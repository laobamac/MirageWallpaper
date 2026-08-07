module;

#include <rstd/macro.hpp>
module sr.scene;
import eigen;
import rstd;
import rstd.log;
import rstd.cppstd;
import sr.utils;

using namespace sr;
using namespace Eigen;

namespace
{
Matrix4d NodeCameraFrame(SceneNode& node) {
    node.UpdateTrans();

    Matrix4d frame = node.ModelTrans();
    if (! frame.allFinite()) return Matrix4d::Identity();

    constexpr double kAxisEps = 1e-10;
    Vector3d         x        = frame.block<3, 1>(0, 0);
    Vector3d         y        = frame.block<3, 1>(0, 1);
    Vector3d         z        = frame.block<3, 1>(0, 2);

    if (! z.allFinite() || z.squaredNorm() <= kAxisEps) {
        z = x.cross(y);
        if (! z.allFinite() || z.squaredNorm() <= kAxisEps) z = Vector3d::UnitZ();
        frame.block<3, 1>(0, 2) = z.normalized();
    }

    if (! frame.allFinite() || std::abs(frame.determinant()) <= kAxisEps)
        return Matrix4d::Identity();
    return frame;
}
} // namespace

Vector3d SceneCamera::GetPosition(SceneRenderViewKind view) const {
    Vector3d position = Vector3d::Zero();
    if (m_lookat) {
        position = m_eye;
    } else if (m_node) {
        position = Affine3d(m_node->GetLocalTrans()) * Vector3d::Zero();
    }
    if (view == SceneRenderViewKind::Reflection) position.y() = -position.y();
    return position;
}

Vector3d SceneCamera::GetDirection() const {
    if (m_lookat) return (m_center - m_eye).normalized();
    if (m_node) {
        return (m_node->GetLocalTrans() * Vector4d(0.0f, 0.0f, -1.0f, 0.0f)).head<3>();
    }
    return -Vector3d::UnitZ();
}

SceneCameraTransforms SceneCamera::Transforms() const {
    if (m_lookat) return SceneCameraTransforms { .eye = m_eye, .center = m_center, .up = m_up };
    if (m_node) {
        const Matrix4d frame = NodeCameraFrame(*m_node);
        const Vector3d eye   = frame.block<3, 1>(0, 3);
        return SceneCameraTransforms {
            .eye    = eye,
            .center = eye - frame.block<3, 1>(0, 2),
            .up     = frame.block<3, 1>(0, 1),
        };
    }
    return {};
}

bool SceneCamera::SetTransforms(const SceneCameraTransforms& transforms) {
    const Vector3d direction = transforms.center - transforms.eye;
    if (! transforms.eye.allFinite() || ! transforms.center.allFinite() ||
        ! transforms.up.allFinite() || direction.squaredNorm() <= 1e-20 ||
        transforms.up.squaredNorm() <= 1e-20 ||
        direction.cross(transforms.up).squaredNorm() <= 1e-20)
        return false;
    SetLookAt(transforms.eye, transforms.center, transforms.up);
    Update();
    return true;
}

Matrix4d SceneCamera::GetViewMatrix() {
    CalculateViewProjectionMatrix();
    return m_viewMat;
}

Matrix4d SceneCamera::GetViewProjectionMatrix(SceneRenderViewKind view) {
    if (view == SceneRenderViewKind::Reflection) return CalculateReflectionViewProjectionMatrix();
    CalculateViewProjectionMatrix();
    return m_viewProjectionMat;
}

Matrix4d SceneCamera::CalculateReflectionViewProjectionMatrix() {
    Vector3d eye    = Vector3d::Zero();
    Vector3d center = -Vector3d::UnitZ();
    Vector3d up     = Vector3d::UnitY();
    if (m_lookat) {
        eye    = m_eye;
        center = m_center;
        up     = m_up;
    } else if (m_node) {
        const Matrix4d frame = NodeCameraFrame(*m_node);
        eye                  = frame.block<3, 1>(0, 3);
        center               = eye - frame.block<3, 1>(0, 2);
        up                   = frame.block<3, 1>(0, 1);
    }
    eye.y()    = -eye.y();
    center.y() = -center.y();
    // Wallpaper Engine keeps camera-up unchanged so the reflected image is
    // screen-upright while the eye and look-at point mirror across Y=0.
    const Matrix4d view = LookAt(eye, center, up);
    if (m_perspective) {
        return Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip) * view;
    }
    return Ortho(-m_width / 2.0,
                 m_width / 2.0,
                 -m_height / 2.0,
                 m_height / 2.0,
                 m_nearClip,
                 m_farClip) *
           view;
}

void SceneCamera::CalculateViewProjectionMatrix() {
    if (m_lookat) {
        m_viewMat = LookAt(m_eye, m_center, m_up);
    } else if (m_node) {
        // view = inv(node.ModelTrans()) so the layer-local frame maps to
        // view origin regardless of where the node sits in the world (parent
        // chain + local translate / scale / rotate). With LookAt-only the
        // node's local scale would leak into clip space and a 9× scaled
        // layer would only see 1/9 of its quad inside the ortho viewport.
        m_viewMat = NodeCameraFrame(*m_node).inverse();
    } else
        m_viewMat = Matrix4d::Identity();

    if (m_perspective) {
        m_viewProjectionMat =
            Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip) * m_viewMat;
    } else {
        double left         = -m_width / 2.0f;
        double right        = m_width / 2.0f;
        double bottom       = -m_height / 2.0f;
        double up           = m_height / 2.0f;
        m_viewProjectionMat = Ortho(left, right, bottom, up, m_nearClip, m_farClip) * m_viewMat;
    }
}

void SceneCamera::Update() { CalculateViewProjectionMatrix(); }

void SceneCamera::AttatchNode(SceneNode* node) {
    if (! node) {
        rstd_error("Attach a null node to camera");
        return;
    }
    m_node   = node;
    m_lookat = false; // node-based view takes over from any explicit LookAt
}
