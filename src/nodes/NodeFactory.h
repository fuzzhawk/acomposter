// Registry of everything that can be dropped on the canvas.
//
// The patcher's palette is generated from this, and the patch loader uses it to
// rebuild a saved graph. VST plugins are not registered here - a plugin node is
// constructed from its own descriptor - so the loader consults a separate hook
// for those.
#pragma once

#include "../core/Node.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace acm {

struct NodeTypeInfo {
    std::string typeName;      // stable key, e.g. "sample.player"
    std::string displayName;   // "Sample Player"
    std::string description;   // one line, shown in the palette and as a tooltip
    NodeCategory category = NodeCategory::Effect;
    std::string paletteGroup;  // heading in the palette
    int sortOrder = 0;
    std::function<std::unique_ptr<Node>()> create;
};

class NodeFactory {
public:
    static NodeFactory& instance();

    void registerType(NodeTypeInfo info);
    std::unique_ptr<Node> create(std::string_view typeName) const;
    const NodeTypeInfo* find(std::string_view typeName) const;
    const std::vector<NodeTypeInfo>& types() const noexcept { return types_; }

    // The patch loader calls this for node types it does not recognise, which in
    // practice means VST plugin nodes. Returning null makes the loader skip the
    // node and report it, rather than losing the rest of the patch.
    using ExternalLoader = std::function<std::unique_ptr<Node>(const std::string& typeName,
                                                              const JsonValue& state)>;
    void setExternalLoader(ExternalLoader loader) { externalLoader_ = std::move(loader); }
    std::unique_ptr<Node> createExternal(const std::string& typeName, const JsonValue& state) const;

private:
    NodeFactory() = default;

    std::vector<NodeTypeInfo> types_;
    ExternalLoader externalLoader_;
};

// Registers the built-in node library. Called once at startup.
void registerBuiltinNodes();

} // namespace acm
