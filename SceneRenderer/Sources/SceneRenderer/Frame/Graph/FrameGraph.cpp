module;

#include <rstd/macro.hpp>

#include "FrameGraph/Pass.hpp"

module sr.rgraph;
import sr.core;
import rstd.cppstd;

using namespace sr::rg;

RenderGraph::RenderGraph() {}

PassNode* RenderGraph::getPassNode(NodeID id) const {
    if (exists(m_set_passnode, id)) {
        return static_cast<PassNode*>(m_dg.GetNode(id));
    }
    return nullptr;
}

TexNode* RenderGraph::getTexNode(NodeID id) const {
    if (! exists(m_set_passnode, id)) {
        return static_cast<TexNode*>(m_dg.GetNode(id));
    }
    return nullptr;
}

Pass* RenderGraph::getPass(NodeID id) const {
    if (exists(m_map_pass, id)) {
        return m_map_pass.at(id).get();
    }
    return nullptr;
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
                if (target && *target == *active_target) return i;
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
        auto* tex_node = getTexNode(out);
        if (tex_node != nullptr && tex_node->writer() == pass_node) {
            return std::string(tex_node->key());
        }
    }
    return std::nullopt;
}

RenderGraphBuilder::RenderGraphBuilder(RenderGraph& rg): m_rg(rg) {};

void RenderGraphBuilder::setWorkPassNode(PassNode* node) { m_passnode_wip = node; }

void RenderGraphBuilder::markSelfWrite(TexNode* tex) {
    if (tex->version() > 0) return;
    m_rg.addPass<VirtualPass>(
        "virtual pass", PassNode::Type::Virtual, [tex](RenderGraphBuilder& builder, auto&) {
            builder.write(tex);
        });
}
void RenderGraphBuilder::markVirtualWrite(TexNode* tex) {
    if (tex->version() > 0 || tex->writer() != nullptr) return;
    m_rg.addPass<VirtualPass>(
        "virtual pass", PassNode::Type::Virtual, [tex](RenderGraphBuilder& builder, auto&) {
            builder.write(tex);
        });
}

TexNode* RenderGraphBuilder::createTexNode(const TexNode::Desc& desc, bool write) {
    TexNode* node { nullptr };
    if (exists(m_rg.m_key_texnode, desc.key)) {
        auto* old = m_rg.getTexNode(m_rg.m_key_texnode.at(desc.key));
        if (write && old->writer() != nullptr) {
            node = createNewTexNode(desc);
        } else {
            node = old;
        }
    } else {
        node = createNewTexNode(desc);
    }
    rstd_assert(node != nullptr);
    return node;
}

TexNode* RenderGraphBuilder::createNewTexNode(const TexNode::Desc& desc) {
    TexNode* node { nullptr };
    if (exists(m_rg.m_key_texnode, desc.key)) {
        auto* old = m_rg.getTexNode(m_rg.m_key_texnode.at(desc.key));
        node      = TexNode::addNewVersion(m_rg.m_dg, old);
    } else {
        node = TexNode::addTexNode(m_rg.m_dg, desc);
    }
    m_rg.m_key_texnode[desc.key] = node->ID();
    return node;
}

void RenderGraphBuilder::read(TexNode* texnode) {
    m_rg.m_dg.Connect(texnode->ID(), m_passnode_wip->ID());

    // reader before all new version's writer
    auto* next = texnode->nextVer();
    if (next != nullptr && next->m_writer != nullptr) {
        m_rg.m_dg.Connect(m_passnode_wip->ID(), next->m_writer->ID());
    }
}

void RenderGraphBuilder::write(TexNode* node) {
    // after all old reader
    if (node->version() > 0) {
        auto*       old  = node->preVer();
        const auto& outs = m_rg.m_dg.GetNodeOut(old->ID());
        // after reader
        for (auto id : outs) {
            if (m_rg.isPassNode(id)) {
                m_rg.m_dg.Connect(id, m_passnode_wip->ID());
            }
        }
        // after old tex if no old reader
        if (outs.empty()) m_rg.m_dg.Connect(old->ID(), m_passnode_wip->ID());
    }
    m_rg.m_dg.Connect(m_passnode_wip->ID(), node->ID());
    node->setWriter(m_passnode_wip);
}

const PassNode& RenderGraphBuilder::workPassNode() const { return *m_passnode_wip; }

std::vector<std::vector<TexNode*>>
RenderGraph::getLastReadTexs(std::span<const NodeID> nodes) const {
    std::vector<std::vector<TexNode*>> res;
    std::vector<Set<NodeID>>           nodes_ids;
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
        std::vector<TexNode*> texs;
        for (auto& id : ids) {
            auto* tex = getTexNode(id);
            if (tex != nullptr) texs.push_back(tex);
        }
        return texs;
    });
    return res;
}
