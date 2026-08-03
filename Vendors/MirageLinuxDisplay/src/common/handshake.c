#define _GNU_SOURCE

#include "handshake.h"

#include "net.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int prepare_handshake(md_client_t* client, uint16_t opcode) {
    md_writer_t writer;
    md_writer_init(&writer, client->handshake_payload, sizeof(client->handshake_payload));
    int rc;
    switch (opcode) {
    case MD_OP_HELLO:
        rc = md_proto_encode_hello(&writer, client->role, client->client_name,
                                   client->client_version, client->advertised_features);
        break;
    case MD_OP_REGISTER_OUTPUT:
    case MD_OP_REGISTER_PRODUCER:
        rc = client->ops->encode_register(client->object, &writer);
        break;
    case MD_OP_CONSUMER_CAPS:
        rc = client->ops->encode_caps(client->object, &writer);
        break;
    default:
        return MD_ERR_INVALID;
    }
    if (rc != 0) return rc == -ENOMEM ? MD_ERR_NOMEM : MD_ERR_INVALID;
    client->handshake_opcode = opcode;
    client->handshake_serial = client->next_serial++;
    client->handshake_size = writer.size;
    return MD_OK;
}

static int start_connected_fd(md_client_t* client, int fd) {
    int status_flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);
    if (status_flags < 0 || descriptor_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        return MD_ERR_IO;
    }
    client->fd = fd;
    client->connection_state = MD_CONNECTION_HANDSHAKING;
    client->handshake_state = MD_HANDSHAKE_HELLO_SEND;
    client->disconnected_notified = false;
    client->selected_minor = 0;
    client->negotiated_features = 0;
    int rc = prepare_handshake(client, MD_OP_HELLO);
    return rc == MD_OK ? MD_OK
                       : md_client_disconnect(client, (md_result_t)rc, "cannot encode hello");
}

void md_client_init(md_client_t* client, uint32_t role, const md_client_ops_t* ops,
                    void* object) {
    client->fd = -1;
    client->connection_state = MD_CONNECTION_DISCONNECTED;
    client->handshake_state = MD_HANDSHAKE_IDLE;
    client->selected_minor = 0;
    client->negotiated_features = 0;
    client->next_serial = 1;
    client->disconnected_notified = false;
    client->role = role;
    client->advertised_features = 0;
    client->socket_path = NULL;
    client->client_name = NULL;
    client->client_version = NULL;
    client->handshake_opcode = 0;
    client->handshake_serial = 0;
    client->handshake_size = 0;
    md_outbox_init(&client->outbox);
    client->ops = ops;
    client->object = object;
}

int md_client_set_identity(md_client_t* client, const char* socket_path,
                           const char* client_name, const char* client_version) {
    if (socket_path == NULL || client_name == NULL || client_version == NULL) {
        return MD_ERR_INVALID;
    }
    md_client_clear_identity(client);
    client->socket_path = md_strdup(socket_path);
    client->client_name = md_strdup(client_name);
    client->client_version = md_strdup(client_version);
    if (client->socket_path == NULL || client->client_name == NULL ||
        client->client_version == NULL) {
        md_client_clear_identity(client);
        return MD_ERR_NOMEM;
    }
    return MD_OK;
}

void md_client_clear_identity(md_client_t* client) {
    free(client->socket_path);
    free(client->client_name);
    free(client->client_version);
    client->socket_path = NULL;
    client->client_name = NULL;
    client->client_version = NULL;
}

int md_client_begin_connect(md_client_t* client) {
    if (client->connection_state != MD_CONNECTION_DISCONNECTED) return MD_ERR_STATE;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return MD_ERR_IO;
    struct sockaddr_un address;
    socklen_t address_length;
    int rc = md_fill_unix_address(client->socket_path, &address, &address_length);
    if (rc != MD_OK) {
        close(fd);
        return rc;
    }

    client->fd = fd;
    client->connection_state = MD_CONNECTION_CONNECTING;
    client->handshake_state = MD_HANDSHAKE_CONNECTING;
    client->disconnected_notified = false;
    client->selected_minor = 0;
    client->negotiated_features = 0;

    if (connect(fd, (struct sockaddr*)&address, address_length) == 0) {
        return start_connected_fd(client, fd);
    }
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EALREADY) {
        return md_client_disconnect(client, MD_ERR_IO, strerror(errno));
    }
    return MD_OK;
}

int md_client_begin_connected_fd(md_client_t* client, int connected_fd) {
    if (connected_fd < 0 || client->connection_state != MD_CONNECTION_DISCONNECTED) {
        return MD_ERR_STATE;
    }
    return start_connected_fd(client, connected_fd);
}

static int send_handshake(md_client_t* client) {
    const uint16_t minor = client->handshake_opcode == MD_OP_HELLO ? 0 : client->selected_minor;
    int rc = md_codec_send(client->fd, minor, client->handshake_opcode, 0,
                           client->handshake_serial, client->handshake_payload,
                           client->handshake_size, NULL, 0);
    if (rc == 1) return MD_HANDSHAKE_NEED_WRITE;
    if (rc < 0) {
        return md_client_disconnect(client, md_map_io_error(rc), "handshake send failed");
    }
    return MD_HANDSHAKE_PROGRESS;
}

static int receive_handshake(md_client_t* client, uint16_t expected, md_packet_t* packet) {
    int rc = md_codec_recv(client->fd, packet);
    if (rc == 0) return MD_HANDSHAKE_NEED_READ;
    if (rc < 0) {
        return md_client_disconnect(client, md_map_io_error(rc), "handshake receive failed");
    }
    if (packet->fd_count != 0 || packet->minor > MIRAGE_DISPLAY_PROTOCOL_MINOR) {
        md_packet_close_fds(packet);
        return md_client_disconnect(client, MD_ERR_PROTOCOL, "invalid handshake packet");
    }
    if (packet->opcode == MD_OP_ERROR) {
        md_proto_error_t error;
        if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
            return md_client_disconnect(client, MD_ERR_PROTOCOL, "malformed error packet");
        }
        md_result_t reason = error.fatal ? MD_ERR_PROTOCOL : MD_ERR_IO;
        const char* message = error.message != NULL ? error.message : "broker error";
        int result = md_client_disconnect(client, reason, message);
        md_proto_error_clear(&error);
        return result;
    }
    if (packet->opcode != expected) {
        return md_client_disconnect(client, MD_ERR_PROTOCOL, "unexpected handshake opcode");
    }
    return MD_HANDSHAKE_PROGRESS;
}

int md_client_advance_handshake(md_client_t* client) {
    if (client == NULL || client->fd < 0) return MD_ERR_STATE;
    int rc;
    switch (client->handshake_state) {
    case MD_HANDSHAKE_CONNECTING: {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(client->fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
            return md_client_disconnect(client, MD_ERR_IO, "getsockopt(SO_ERROR) failed");
        }
        if (error == EINPROGRESS || error == EALREADY) return MD_HANDSHAKE_NEED_WRITE;
        if (error != 0) return md_client_disconnect(client, MD_ERR_IO, strerror(error));
        rc = prepare_handshake(client, MD_OP_HELLO);
        if (rc != MD_OK) {
            return md_client_disconnect(client, (md_result_t)rc, "cannot encode hello");
        }
        client->connection_state = MD_CONNECTION_HANDSHAKING;
        client->handshake_state = MD_HANDSHAKE_HELLO_SEND;
        return MD_HANDSHAKE_PROGRESS;
    }
    case MD_HANDSHAKE_HELLO_SEND:
        rc = send_handshake(client);
        if (rc == MD_HANDSHAKE_PROGRESS) client->handshake_state = MD_HANDSHAKE_WELCOME_WAIT;
        return rc;
    case MD_HANDSHAKE_WELCOME_WAIT: {
        md_packet_t packet;
        rc = receive_handshake(client, MD_OP_WELCOME, &packet);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        md_proto_welcome_t welcome;
        if (md_proto_decode_welcome(packet.payload, packet.payload_size, &welcome) != 0 ||
            welcome.selected_minor > MIRAGE_DISPLAY_PROTOCOL_MINOR ||
            (welcome.features & MD_FEATURE_EXPLICIT_SYNC) == 0) {
            return md_client_disconnect(client, MD_ERR_PROTOCOL, "unsupported welcome packet");
        }
        client->selected_minor = welcome.selected_minor;
        client->negotiated_features = client->advertised_features & welcome.features;
        md_proto_welcome_clear(&welcome);
        rc = prepare_handshake(client, client->ops->register_opcode);
        if (rc != MD_OK) {
            return md_client_disconnect(client, (md_result_t)rc, "cannot encode registration");
        }
        client->handshake_state = MD_HANDSHAKE_REGISTER_SEND;
        return MD_HANDSHAKE_PROGRESS;
    }
    case MD_HANDSHAKE_REGISTER_SEND:
        rc = send_handshake(client);
        if (rc == MD_HANDSHAKE_PROGRESS) client->handshake_state = MD_HANDSHAKE_ACCEPT_WAIT;
        return rc;
    case MD_HANDSHAKE_ACCEPT_WAIT: {
        md_packet_t packet;
        rc = receive_handshake(client, client->ops->accepted_opcode, &packet);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        if (client->ops->apply_accepted(client->object, &packet) != 0) {
            return md_client_disconnect(client, MD_ERR_PROTOCOL, "malformed accepted packet");
        }
        if (client->ops->caps_opcode != 0) {
            rc = prepare_handshake(client, client->ops->caps_opcode);
            if (rc != MD_OK) {
                return md_client_disconnect(client, (md_result_t)rc, "cannot encode caps");
            }
            client->handshake_state = MD_HANDSHAKE_CAPS_SEND;
            return MD_HANDSHAKE_PROGRESS;
        }
        client->handshake_state = MD_HANDSHAKE_READY;
        client->connection_state = MD_CONNECTION_READY;
        if (client->ops->on_ready != NULL) client->ops->on_ready(client->object);
        return MD_HANDSHAKE_DONE;
    }
    case MD_HANDSHAKE_CAPS_SEND:
        rc = send_handshake(client);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        client->handshake_state = MD_HANDSHAKE_READY;
        client->connection_state = MD_CONNECTION_READY;
        if (client->ops->on_ready != NULL) client->ops->on_ready(client->object);
        return MD_HANDSHAKE_DONE;
    case MD_HANDSHAKE_READY:
        return MD_HANDSHAKE_DONE;
    default:
        return MD_ERR_STATE;
    }
}

static int64_t monotonic_millis(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

int md_client_connect(md_client_t* client, int timeout_ms) {
    int rc = md_client_begin_connect(client);
    if (rc != MD_OK) return rc;
    int64_t start = monotonic_millis();
    for (;;) {
        rc = md_client_advance_handshake(client);
        if (rc == MD_HANDSHAKE_DONE) return MD_OK;
        if (rc < 0) return rc;
        if (rc == MD_HANDSHAKE_PROGRESS) continue;
        short events = rc == MD_HANDSHAKE_NEED_WRITE ? POLLOUT : POLLIN;
        int wait_ms = timeout_ms;
        if (timeout_ms >= 0 && start >= 0) {
            int64_t elapsed = monotonic_millis() - start;
            if (elapsed >= timeout_ms) {
                md_client_close(client);
                return MD_ERR_IO;
            }
            wait_ms = timeout_ms - (int)elapsed;
        }
        struct pollfd pfd = {.fd = client->fd, .events = events, .revents = 0};
        int poll_rc;
        do { poll_rc = poll(&pfd, 1, wait_ms); } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc <= 0) {
            md_client_close(client);
            return MD_ERR_IO;
        }
    }
}

void md_client_close(md_client_t* client) {
    if (client->fd >= 0) {
        uint8_t payload[4];
        md_writer_t writer;
        md_writer_init(&writer, payload, sizeof(payload));
        if (client->connection_state == MD_CONNECTION_READY &&
            md_proto_encode_u32(&writer, 0) == 0) {
            (void)md_codec_send(client->fd, client->selected_minor, MD_OP_GOODBYE, 0,
                                client->next_serial++, payload, writer.size, NULL, 0);
        }
        close(client->fd);
    }
    client->fd = -1;
    md_outbox_clear(&client->outbox);
    client->connection_state = MD_CONNECTION_DISCONNECTED;
    client->handshake_state = MD_HANDSHAKE_IDLE;
    client->selected_minor = 0;
    client->negotiated_features = 0;
    client->disconnected_notified = false;
}

int md_client_disconnect(md_client_t* client, md_result_t reason, const char* message) {
    if (client->fd >= 0) close(client->fd);
    client->fd = -1;
    md_outbox_clear(&client->outbox);
    client->connection_state = MD_CONNECTION_DEAD;
    client->handshake_state = MD_HANDSHAKE_IDLE;
    if (client->ops->notify_disconnected != NULL) {
        client->ops->notify_disconnected(client->object, reason,
                                         message != NULL ? message : "session failed");
    }
    return reason;
}

int md_client_flush_outbox(md_client_t* client) {
    return md_outbox_flush(&client->outbox, client->fd, client->selected_minor);
}
