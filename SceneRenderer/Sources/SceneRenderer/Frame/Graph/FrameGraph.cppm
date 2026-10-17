module;

#include <memory>
#include "FrameGraph/Pass.hpp"

export module sr.rgraph:render_graph;
import sr.core;
import rstd.cppstd;

import :dependency_graph;
import :pass_node;

export namespace sr::rg
{

class RenderGraph;

struct TextureNodeRef {
    NodeID id { std::numeric_limits<NodeID>::max() };

    bool valid() const { return id != std::numeric_limits<NodeID>::max(); }
};

enum class TextureKind
{
    Imported,
    Temp,
};

struct TextureDesc {
    std::string name;
    std::string key;
    TextureKind kind { TextureKind::Imported };
};

struct TextureNodeState {
    TextureNodeRef ref;
    TextureDesc    desc;
    size_t         version { 0 };
};

struct PassNodeState {
    NodeID         id { 0 };
    std::string    name;
    PassNode::Type type { PassNode::Type::CustomShader };
};

class RenderGraphBuilder {
public:
    RenderGraphBuilder(RenderGraph&);

    TextureNodeRef                  createTexture(const TextureDesc&, bool write = false);
    void                            read(TextureNodeRef);
    void                            write(TextureNodeRef);
    std::optional<TextureNodeState> textureState(TextureNodeRef) const;
    const PassNode&                 workPassNode() const;
    void                            setWorkPassNode(PassNode*);
    void                            markSelfWrite(TextureNodeRef);
    void                            markVirtualWrite(TextureNodeRef);

private:
    TextureNodeRef createTextureNode(const TextureDesc&, bool write);
    void           readTextureNode(TextureNodeRef);
    void           writeTextureNode(TextureNodeRef);

    RenderGraph& m_rg;
    PassNode*    m_passnode_wip { nullptr };
};

class RenderGraph {
public:
    RenderGraph();

    Pass*                           getPass(NodeID) const;
    std::optional<PassNodeState>    passState(NodeID) const;
    std::optional<TextureNodeState> textureState(TextureNodeRef) const;
    bool                            readTexture(NodeID pass_node_id, TextureNodeRef texture);

    // all render pass
    std::vector<NodeID>                        topologicalOrder() const;
    std::vector<std::vector<TextureNodeState>> getLastReadTextures(std::span<const NodeID>) const;

    void ToGraphviz(std::string_view path) const;

    template<typename TPass, typename CB>
    PassNode* addPass(std::string_view name, PassNode::Type type, CB&& callback) {
        using Desc = typename TPass::Desc;

        auto* node = PassNode::addPassNode(m_dg, type);
        node->setName(name);
        markPassNode(node->ID());
        RenderGraphBuilder builder(*this);
        builder.setWorkPassNode(node);
        {
            Desc desc {};
            callback(builder, desc);
            m_map_pass[node->ID()] = std::make_shared<TPass>(desc);
        }
        if (type == PassNode::Type::Virtual) m_set_vitrual_passnode.insert(node->ID());
        return node;
    }

private:
    friend class RenderGraphBuilder;
    PassNode*                  getPassNode(NodeID) const;
    TextureNodeRef             createTextureNode(const TextureDesc&, bool write);
    TextureNodeRef             createNewTextureNode(const TextureDesc&);
    void                       connectTextureRead(TextureNodeRef, NodeID pass_node_id);
    void                       connectTextureWrite(TextureNodeRef, NodeID pass_node_id);
    bool                       textureHasWriter(TextureNodeRef) const;
    void                       markPassNode(NodeID);
    bool                       isPassNode(NodeID) const;
    bool                       isVirtualPassNode(NodeID) const;
    bool                       isRenderPassNode(NodeID) const;
    std::optional<std::string> passWriteTarget(NodeID) const;

    DependencyGraph m_dg;
    Set<NodeID>     m_set_passnode;
    Set<NodeID>     m_set_vitrual_passnode;

    Map<std::string, NodeID> m_key_texnode;

    Map<NodeID, std::shared_ptr<Pass>> m_map_pass;
};

} // namespace sr::rg
