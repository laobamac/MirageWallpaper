module;

export module sr.rgraph:pass_node;
import rstd.cppstd;

import :dependency_graph;

export namespace sr::rg
{

class TexNode;
class PassNode : public DependencyGraph::Node {
public:
    enum class Type
    {
        CustomShader,
        Copy,
        Virtual // for mark a virual writer to update version
    };
    static PassNode* addPassNode(DependencyGraph& dg, Type type);

    Type             type() const;
    std::string_view name() const;

    void setName(std::string_view);

    std::string ToGraphviz() const override;

private:
    Type        m_type;
    std::string m_name { "unknown pass" };
};

} // namespace sr::rg
