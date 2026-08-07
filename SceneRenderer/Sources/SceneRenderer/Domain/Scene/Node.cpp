module;

module sr.scene;
import eigen;
import rstd.cppstd;

using namespace sr;
using namespace Eigen;

Matrix4d SceneNode::GetLocalTrans() const {
    Affine3d trans = Affine3d::Identity();
    trans.prescale(m_scale.cast<double>());

    // m_rotation is in radians. Static scene.json `angles` are already radians;
    // the JS scripting API uses degrees and converts at the boundary (Script.cpp
    // NodeSetAngles / the transform actuator), so everything stored here is rad.
    trans.prerotate(AngleAxis<double>(m_rotation.x(), Vector3d::UnitX())); // x
    trans.prerotate(AngleAxis<double>(m_rotation.y(), Vector3d::UnitY())); // y
    trans.prerotate(AngleAxis<double>(m_rotation.z(), Vector3d::UnitZ())); // z

    trans.pretranslate(m_translate.cast<double>());

    return m_local_frame * trans.matrix();
}

void SceneNode::RotateObjectSpace(const Vector3f& rotation) {
    const Quaternionf current = AngleAxisf(m_rotation.z(), Vector3f::UnitZ()) *
                                AngleAxisf(m_rotation.y(), Vector3f::UnitY()) *
                                AngleAxisf(m_rotation.x(), Vector3f::UnitX());
    const Quaternionf local = AngleAxisf(rotation.z(), Vector3f::UnitZ()) *
                              AngleAxisf(rotation.y(), Vector3f::UnitY()) *
                              AngleAxisf(rotation.x(), Vector3f::UnitX());
    const Matrix3f composed = (current * local).toRotationMatrix();
    const float    y = std::asin(std::clamp(-composed(2, 0), -1.0f, 1.0f));
    float          x {};
    float          z {};
    if (std::abs(std::cos(y)) > 1e-6f) {
        x = std::atan2(composed(2, 1), composed(2, 2));
        z = std::atan2(composed(1, 0), composed(0, 0));
    } else {
        // At gimbal lock only x-z (or x+z) is observable. Choose z=0 and
        // retain the equivalent orientation in x.
        x = std::atan2(-composed(1, 2), composed(1, 1));
    }
    SetRotation({ x, y, z });
}

void SceneNode::UpdateTrans() {
    if (! m_dirty) return;
    m_dirty = false;

    if (m_parent) {
        m_parent->UpdateTrans();
    }
    {
        Affine3d trans = Affine3d::Identity();
        if (m_parent) {
            trans *= m_parent->ModelTrans();
        }
        m_trans = (trans * GetLocalTrans()).matrix();
    }
}

void SceneNode::MarkTransDirty() {
    if (! m_dirty) {
        m_dirty = true;
        for (auto& child : m_children) {
            child->MarkTransDirty();
        }
        for (auto* anchor : m_transform_anchors) {
            if (anchor) anchor->MarkTransDirty();
        }
    }
}

SceneNode* SceneNode::FindByName(std::string_view name) {
    if (m_name == name) return this;
    for (auto& child : m_children) {
        if (auto* hit = child->FindByName(name)) return hit;
    }
    return nullptr;
}
