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

    return trans.matrix();
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
