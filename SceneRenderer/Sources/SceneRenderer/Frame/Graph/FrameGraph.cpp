module;

#include <rstd/macro.hpp>

#include "FrameGraph/Pass.hpp"

module sr.rgraph;
import sr.core;
import rstd.cppstd;
import :tex_node;

using namespace sr::rg;

RenderGraph::RenderGraph() {}

namespace
{
TexNode::Desc ToTexNodeDesc(const TextureDesc& desc) {
    return TexNode::Desc {
        .name = desc.name,
        .key  = desc.key,
        .type =
            desc.kind == TextureKind::Temp ? TexNode::TexType::Temp : TexNode::TexType::Imported,
    };
}

TextureKind ToTextureKind(TexNode::TexType type) {
    switch (type) {
    case TexNode::TexType::Imported: return TextureKind::Imported;
    case TexNode::TexType::Temp: return TextureKind::Temp;
    }
    return TextureKind::Imported;
}

TextureDesc ToTextureDesc(const TexNode& node) {
    return TextureDesc {
        .name = std::string(node.name()),
        .key  = std::string(node.key()),
        .kind = ToTextureKind(node.type()),
    };
}

std::string DotEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\': out += R"(\\)"; break;
        case '"': out += R"(\")"; break;
        case '\n': out += R"(\n)"; break;
        case '\r': break;
        default: out += c; break;
        }
    }
    return out;
}

std::string PassTypeName(PassNode::Type type) {
    switch (type) {
    case PassNode::Type::CustomShader: return "CustomShader";
    case PassNode::Type::Copy: return "Copy";
    case PassNode::Type::Virtual: return "Virtual";
    }
    return "Unknown";
}

std::string TextureTypeName(TexNode::TexType type) {
    switch (type) {
    case TexNode::TexType::Imported: return "Imported";
    case TexNode::TexType::Temp: return "Temp";
    }
    return "Unknown";
}

std::string PassDebugLabel(const PassNode& node) {
    return "ref=" + node.GraphID() + R"(\npass: )" + DotEscape(node.name()) + R"(\ntype=)" +
           PassTypeName(node.type());
}

std::string TextureDebugLabel(const TexNode& node) {
    std::string label = "ref=" + node.GraphID() + R"(\nresource: )" + DotEscape(node.name()) +
                        R"(\nkey=)" + DotEscape(node.key()) + R"(\nkind=)" +
                        TextureTypeName(node.type()) + R"(\nversion=)" +
                        std::to_string(node.version());
    if (auto* writer = node.writer()) {
        label += R"(\nwriter=)" + writer->GraphID() + " " + DotEscape(writer->name());
    }
    if (auto* pre = node.preVer()) label += R"(\nprev=)" + pre->GraphID();
    if (auto* next = node.nextVer()) label += R"(\nnext=)" + next->GraphID();
    return label;
}
} // namespace

PassNode* RenderGraph::getPassNode(NodeID id) const {
    if (exists(m_set_passnode, id)) {
        return static_cast<PassNode*>(m_dg.GetNode(id));
    }
    return nullptr;
}

Pass* RenderGraph::getPass(NodeID id) const {
    if (exists(m_map_pass, id)) {
        return m_map_pass.at(id).get();
    }
    return nullptr;
}

std::optional<PassNodeState> RenderGraph::passState(NodeID id) const {
    auto* node = getPassNode(id);
    if (node == nullptr) return std::nullopt;
    return PassNodeState {
        .id   = id,
        .name = std::string(node->name()),
        .type = node->type(),
    };
}

void RenderGraph::ToGraphviz(std::string_view path) const {
    std::ofstream fs;
    fs.open(std::string(path), std::fstream::out | std::fstream::trunc);
    if (! fs.is_open()) return;

    fs << "digraph framegraph {\n";
    fs << "node [shape=box]\n";

    auto texture_node = [this](NodeID id) -> TexNode* {
        if (id >= m_dg.NodeNum() || exists(m_set_passnode, id)) return nullptr;
        return static_cast<TexNode*>(m_dg.GetNode(id));
    };

    for (NodeID id = 0; id < m_dg.NodeNum(); ++id) {
        if (auto* pass = getPassNode(id)) {
            fs << pass->GraphID() << R"([label=")" << PassDebugLabel(*pass) << R"("])" << '\n';
            continue;
        }
        if (auto* tex = texture_node(id)) {
            fs << tex->GraphID() << R"([label=")" << TextureDebugLabel(*tex)
               << R"(" shape=ellipse])" << '\n';
        }
    }

    for (NodeID id = 0; id < m_dg.NodeNum(); ++id) {
        auto outs = m_dg.GetNodeOut(id);
        std::sort(outs.begin(), outs.end());
        for (auto out : outs) {
            std::string access = "order";
            if (auto* tex = texture_node(id); tex != nullptr && getPassNode(out) != nullptr) {
                access = tex->nextVer() != nullptr && tex->nextVer()->writer() == getPassNode(out)
                             ? "read/version"
                             : "read";
            } else if (auto* pass = getPassNode(id); pass != nullptr) {
                if (auto* tex = texture_node(out); tex != nullptr && tex->writer() == pass) {
                    access = "write";
                }
            }
            fs << "n" << id << "->n" << out << R"([label="access=)" << access << R"("])" << '\n';
        }
    }

    fs << "}";
}

std::optional<TextureNodeState> RenderGraph::textureState(TextureNodeRef ref) const {
    if (ref.id >= m_dg.NodeNum() || isPassNode(ref.id)) return std::nullopt;
    auto* node = static_cast<TexNode*>(m_dg.GetNode(ref.id));
    return TextureNodeState {
        .ref     = ref,
        .desc    = ToTextureDesc(*node),
        .version = node->version(),
    };
}

bool RenderGraph::readTexture(NodeID pass_node_id, TextureNodeRef texture) {
    auto* pass_node = getPassNode(pass_node_id);
    auto  tex_state = textureState(texture);
    if (pass_node == nullptr || ! tex_state.has_value()) return false;

    RenderGraphBuilder builder(*this);
    builder.setWorkPassNode(pass_node);
    builder.read(texture);
    return true;
}

std::vector<NodeID> RenderGraph::topologicalOrder() const {
    const size_t node_count = m_dg.NodeNum();

    std::vector<size_t> in_degree(node_count, 0);
    for (NodeID id = 0; id < node_count; ++id) {
        for (auto out : m_dg.GetNodeOut(id)) {
            ++in_degree[out];
        }
    }

    std::vector<NodeID> ready;
    ready.reserve(node_count);
    for (NodeID id = 0; id < node_count; ++id) {
        if (in_degree[id] == 0) ready.push_back(id);
    }

    std::vector<NodeID>        passnodes;
    std::optional<std::string> active_target;
    size_t                     visited = 0;

    auto chooseReady = [&]() -> size_t {
        std::sort(ready.begin(), ready.end());

        for (size_t i = 0; i < ready.size(); ++i) {
            if (! isRenderPassNode(ready[i])) return i;
        }

        if (active_target) {
            for (size_t i = 0; i < ready.size(); ++i) {
                auto target = passWriteTarget(ready[i]);
                if (target && *target->c_str() == *active_target->c_str()) return i;
            }
        }

        std::unordered_map<std::string, size_t> target_counts;
        for (auto id : ready) {
            if (auto target = passWriteTarget(id)) {
                ++target_counts[*target];
            }
        }

        size_t best_index = 0;
        size_t best_count = 0;
        for (size_t i = 0; i < ready.size(); ++i) {
            size_t count = 0;
            if (auto target = passWriteTarget(ready[i])) {
                count = target_counts[*target];
            }
            if (count > best_count) {
                best_index = i;
                best_count = count;
            }
        }
        return best_index;
    };

    while (! ready.empty()) {
        const size_t pick_index = chooseReady();
        const NodeID id         = ready[pick_index];
        ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(pick_index));
        ++visited;

        if (isRenderPassNode(id)) {
            passnodes.push_back(id);
            active_target = passWriteTarget(id);
        }

        auto outs = m_dg.GetNodeOut(id);
        std::sort(outs.begin(), outs.end());
        for (auto out : outs) {
            rstd_assert(in_degree[out] > 0);
            --in_degree[out];
            if (in_degree[out] == 0) ready.push_back(out);
        }
    }

    if (visited == node_count) return passnodes;

    std::vector<NodeID> allnodes = m_dg.TopologicalOrder();
    passnodes.clear();
    std::copy_if(
        allnodes.begin(), allnodes.end(), std::back_inserter(passnodes), [this](auto item) {
            return isRenderPassNode(item);
        });
    return passnodes;
}

void RenderGraph::markPassNode(NodeID id) { m_set_passnode.insert(id); }

bool RenderGraph::isPassNode(NodeID id) const { return exists(m_set_passnode, id); }

bool RenderGraph::isVirtualPassNode(NodeID id) const { return exists(m_set_vitrual_passnode, id); }

bool RenderGraph::isRenderPassNode(NodeID id) const {
    return isPassNode(id) && ! isVirtualPassNode(id);
}

std::optional<std::string> RenderGraph::passWriteTarget(NodeID id) const {
    auto* pass_node = getPassNode(id);
    if (pass_node == nullptr) return std::nullopt;

    auto outs = m_dg.GetNodeOut(id);
    std::sort(outs.begin(), outs.end());
    for (auto out : outs) {
        if (out >= m_dg.NodeNum() || isPassNode(out)) continue;
        auto* tex_node = static_cast<TexNode*>(m_dg.GetNode(out));
        if (tex_node->writer() == pass_node) {
            return std::string(tex_node->key());
        }
    }
    return std::nullopt;
}

RenderGraphBuilder::RenderGraphBuilder(RenderGraph& rg): m_rg(rg) {};

void RenderGraphBuilder::setWorkPassNode(PassNode* node) { m_passnode_wip = node; }

void RenderGraphBuilder::markSelfWrite(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.has_value());
    if (! state.has_value()) return;
    if (state->version > 0) return;
    m_rg.addPass<VirtualPass>(
        "virtual pass", PassNode::Type::Virtual, [ref](RenderGraphBuilder& builder, auto&) {
            builder.write(ref);
        });
}

void RenderGraphBuilder::markVirtualWrite(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.has_value());
    if (! state.has_value()) return;
    if (state->version > 0 || m_rg.textureHasWriter(ref)) return;
    m_rg.addPass<VirtualPass>(
        "virtual pass", PassNode::Type::Virtual, [ref](RenderGraphBuilder& builder, auto&) {
            builder.write(ref);
        });
}

TextureNodeRef RenderGraphBuilder::createTexture(const TextureDesc& desc, bool write) {
    return createTextureNode(desc, write);
}

void RenderGraphBuilder::read(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.has_value());
    if (! state.has_value()) return;
    readTextureNode(ref);
}

void RenderGraphBuilder::write(TextureNodeRef ref) {
    auto state = m_rg.textureState(ref);
    rstd_assert(state.has_value());
    if (! state.has_value()) return;
    writeTextureNode(ref);
}

std::optional<TextureNodeState> RenderGraphBuilder::textureState(TextureNodeRef ref) const {
    return m_rg.textureState(ref);
}

TextureNodeRef RenderGraphBuilder::createTextureNode(const TextureDesc& desc, bool write) {
    return m_rg.createTextureNode(desc, write);
}

void RenderGraphBuilder::readTextureNode(TextureNodeRef ref) {
    m_rg.connectTextureRead(ref, m_passnode_wip->ID());
}

void RenderGraphBuilder::writeTextureNode(TextureNodeRef ref) {
    m_rg.connectTextureWrite(ref, m_passnode_wip->ID());
}

TextureNodeRef RenderGraph::createTextureNode(const TextureDesc& desc, bool write) {
    TextureNodeRef ref {};
    if (exists(m_key_texnode, desc.key)) {
        auto id  = m_key_texnode.at(desc.key);
        auto old = TextureNodeRef { .id = id };
        if (write && textureHasWriter(old)) {
            ref = createNewTextureNode(desc);
        } else {
            ref = old;
        }
    } else {
        ref = createNewTextureNode(desc);
    }
    rstd_assert(ref.valid());
    return ref;
}

TextureNodeRef RenderGraph::createNewTextureNode(const TextureDesc& desc) {
    auto       legacy_desc = ToTexNodeDesc(desc);
    TexNode*   node { nullptr };
    const auto it = m_key_texnode.find(desc.key);
    if (it != m_key_texnode.end()) {
        auto* old = static_cast<TexNode*>(m_dg.GetNode(it->second));
        node      = TexNode::addNewVersion(m_dg, old);
    } else {
        node = TexNode::addTexNode(m_dg, legacy_desc);
    }
    m_key_texnode[desc.key] = node->ID();
    return TextureNodeRef { .id = node->ID() };
}

void RenderGraph::connectTextureRead(TextureNodeRef ref, NodeID pass_node_id) {
    if (! textureState(ref).has_value() || getPassNode(pass_node_id) == nullptr) return;
    auto* texnode = static_cast<TexNode*>(m_dg.GetNode(ref.id));
    m_dg.Connect(texnode->ID(), pass_node_id);

    // reader before all new version's writer
    auto* next = texnode->nextVer();
    if (next != nullptr && next->writer() != nullptr) {
        m_dg.Connect(pass_node_id, next->writer()->ID());
    }
}

void RenderGraph::connectTextureWrite(TextureNodeRef ref, NodeID pass_node_id) {
    if (! textureState(ref).has_value()) return;
    auto* pass = getPassNode(pass_node_id);
    if (pass == nullptr) return;

    auto* node = static_cast<TexNode*>(m_dg.GetNode(ref.id));
    // after all old reader
    if (node->version() > 0) {
        auto*       old  = node->preVer();
        const auto& outs = m_dg.GetNodeOut(old->ID());
        // after reader
        for (auto id : outs) {
            if (isPassNode(id)) {
                m_dg.Connect(id, pass_node_id);
            }
        }
        // after old tex if no old reader
        if (outs.empty()) m_dg.Connect(old->ID(), pass_node_id);
    }
    m_dg.Connect(pass_node_id, node->ID());
    node->setWriter(pass);
}

bool RenderGraph::textureHasWriter(TextureNodeRef ref) const {
    if (! textureState(ref).has_value()) return false;
    auto* node = static_cast<TexNode*>(m_dg.GetNode(ref.id));
    return node->writer() != nullptr;
}

const PassNode& RenderGraphBuilder::workPassNode() const { return *m_passnode_wip; }

std::vector<std::vector<TextureNodeState>>
RenderGraph::getLastReadTextures(std::span<const NodeID> nodes) const {
    std::vector<std::vector<TextureNodeState>> res;
    std::vector<Set<NodeID>>                   nodes_ids;
    // get in
    std::transform(
        nodes.begin(), nodes.end(), std::back_inserter(nodes_ids), [this, &nodes_ids](auto& n) {
            Set<NodeID> sets;
            const auto& ids = m_dg.GetNodeIn(n);
            for (const auto& id : ids) sets.insert(id);
            return sets;
        });
    // get last in
    {
        Set<NodeID> sets;
        std::for_each(std::rbegin(nodes_ids), std::rend(nodes_ids), [&sets](auto& ids) {
            std::vector<NodeID> copy { ids.begin(), ids.end() };
            std::for_each(copy.begin(), copy.end(), [&sets, &ids](auto& id) {
                if (exists(sets, id))
                    ids.erase(id);
                else
                    sets.insert(id);
            });
        });
    }
    // to tex node
    std::transform(nodes_ids.begin(), nodes_ids.end(), std::back_inserter(res), [this](auto& ids) {
        std::vector<TextureNodeState> texs;
        for (auto& id : ids) {
            auto state = textureState(TextureNodeRef { .id = id });
            if (state.has_value()) texs.push_back(*state);
        }
        return texs;
    });
    return res;
}
