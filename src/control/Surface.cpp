#include "Surface.h"

#include <algorithm>
#include <cmath>

namespace acm::control {
namespace {

const char* kKindNames[] = { "knob", "fader", "button", "xy", "metasurface", "label" };

Parameter* parameterAt(Graph& graph, ParamAddress address) {
    Node* node = graph.node(address.node);
    if (!node) return nullptr;
    if (address.param < 0 || address.param >= node->numParameters()) return nullptr;
    return &node->parameter(address.param);
}

const Parameter* parameterAt(const Graph& graph, ParamAddress address) {
    const Node* node = graph.node(address.node);
    if (!node) return nullptr;
    if (address.param < 0 || address.param >= node->numParameters()) return nullptr;
    return &node->parameter(address.param);
}

JsonValue targetsToJson(const std::vector<Target>& targets) {
    JsonValue array = JsonValue::array();
    for (const Target& target : targets) {
        JsonValue entry = JsonValue::object();
        entry.set("node", static_cast<int>(target.address.node));
        entry.set("param", static_cast<int>(target.address.param));
        entry.set("low", target.low);
        entry.set("high", target.high);
        array.push(std::move(entry));
    }
    return array;
}

std::vector<Target> targetsFromJson(const JsonValue& array) {
    std::vector<Target> targets;
    if (!array.isArray()) return targets;

    for (std::size_t i = 0; i < array.size(); ++i) {
        const JsonValue& entry = array.at(i);
        if (!entry.isObject()) continue;

        Target target;
        target.address.node = static_cast<NodeId>(entry.getInt("node", -1));
        target.address.param = static_cast<ParamIndex>(entry.getInt("param", -1));
        target.low = entry.getFloat("low", 0.0f);
        target.high = entry.getFloat("high", 1.0f);

        if (target.address.valid()) targets.push_back(target);
    }
    return targets;
}

} // namespace

const char* toString(ControlKind kind) noexcept {
    const auto index = static_cast<int>(kind);
    if (index < 0 || index >= static_cast<int>(ControlKind::Count)) return "knob";
    return kKindNames[index];
}

ControlKind controlKindFromString(const std::string& text) noexcept {
    for (int i = 0; i < static_cast<int>(ControlKind::Count); ++i)
        if (text == kKindNames[i]) return static_cast<ControlKind>(i);
    return ControlKind::Knob;
}

Surface::Surface() {
    pages_.push_back(Page{ "main", {} });
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

void Surface::setActivePage(int index) noexcept {
    if (index >= 0 && index < pageCount()) activePage_ = index;
}

int Surface::addPage(std::string name) {
    pages_.push_back(Page{ std::move(name), {} });
    return pageCount() - 1;
}

bool Surface::removePage(int index) {
    // The last page is never removed. A surface with no pages has nowhere to
    // put the next control, and the state that comes back from that is a
    // special case in every function here rather than in this one.
    if (index < 0 || index >= pageCount() || pageCount() <= 1) return false;

    pages_.erase(pages_.begin() + index);
    if (activePage_ >= pageCount()) activePage_ = pageCount() - 1;
    return true;
}

bool Surface::renamePage(int index, std::string name) {
    Page* target = page(index);
    if (!target) return false;
    target->name = std::move(name);
    return true;
}

Page* Surface::page(int index) {
    if (index < 0 || index >= pageCount()) return nullptr;
    return &pages_[static_cast<std::size_t>(index)];
}

const Page* Surface::page(int index) const {
    if (index < 0 || index >= pageCount()) return nullptr;
    return &pages_[static_cast<std::size_t>(index)];
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

Control* Surface::find(int id) {
    Page* current = page(activePage_);
    if (!current) return nullptr;

    for (Control& control : current->controls)
        if (control.id == id) return &control;
    return nullptr;
}

const Control* Surface::find(int id) const {
    const Page* current = page(activePage_);
    if (!current) return nullptr;

    for (const Control& control : current->controls)
        if (control.id == id) return &control;
    return nullptr;
}

int Surface::add(ControlKind kind, std::string name, int column, int row,
                 int width, int height) {
    Page* current = page(activePage_);
    if (!current) return 0;

    Control control;
    control.id = nextId_++;
    control.kind = kind;
    control.name = std::move(name);
    control.column = std::max(0, column);
    control.row = std::max(0, row);
    control.width = std::max(1, width);
    control.height = std::max(1, height);

    current->controls.push_back(std::move(control));
    return current->controls.back().id;
}

bool Surface::remove(int id) {
    Page* current = page(activePage_);
    if (!current) return false;

    const auto it = std::find_if(current->controls.begin(), current->controls.end(),
                                 [id](const Control& c) { return c.id == id; });
    if (it == current->controls.end()) return false;

    current->controls.erase(it);
    return true;
}

bool Surface::move(int id, int column, int row) {
    Control* control = find(id);
    if (!control) return false;

    control->column = clampValue(column, 0, std::max(0, columns_ - control->width));
    control->row = clampValue(row, 0, std::max(0, rows_ - control->height));
    return true;
}

bool Surface::resize(int id, int width, int height) {
    Control* control = find(id);
    if (!control) return false;

    control->width = clampValue(width, 1, columns_ - control->column);
    control->height = clampValue(height, 1, rows_ - control->row);
    return true;
}

void Surface::setGrid(int columns, int rows) noexcept {
    columns_ = clampValue(columns, 4, 48);
    rows_ = clampValue(rows, 3, 32);

    // Anything now outside the grid is pulled back in rather than left where it
    // cannot be seen or clicked.
    for (Page& current : pages_) {
        for (Control& control : current.controls) {
            control.width = std::min(control.width, columns_);
            control.height = std::min(control.height, rows_);
            control.column = clampValue(control.column, 0, columns_ - control.width);
            control.row = clampValue(control.row, 0, rows_ - control.height);
        }
    }
}

// ---------------------------------------------------------------------------
// Driving the graph
// ---------------------------------------------------------------------------

void Surface::applyTargets(const std::vector<Target>& targets, float amount, Graph& graph) {
    for (const Target& target : targets) {
        Parameter* parameter = parameterAt(graph, target.address);
        if (!parameter) continue;
        parameter->setNormalised(clampValue(target.valueFor(amount), 0.0f, 1.0f));
    }
}

bool Surface::setValue(int id, float value, Graph& graph) {
    Control* control = find(id);
    if (!control || !control->drivesParameters()) return false;

    control->value = clampValue(value, 0.0f, 1.0f);
    applyTargets(control->targets, control->value, graph);
    return true;
}

bool Surface::setValueXY(int id, float x, float y, Graph& graph) {
    Control* control = find(id);
    if (!control || !control->hasSecondAxis()) return false;

    control->value = clampValue(x, 0.0f, 1.0f);
    control->valueY = clampValue(y, 0.0f, 1.0f);
    applyTargets(control->targets, control->value, graph);
    applyTargets(control->targetsY, control->valueY, graph);
    return true;
}

void Surface::adoptFromGraph(int id, const Graph& graph) {
    Control* control = find(id);
    if (!control || control->targets.empty()) return;

    // The first target decides, because there is no single answer once a macro
    // has several and they disagree - and the first is the one that was bound
    // first, which is the one the performer thinks of as "the" parameter.
    const Target& target = control->targets.front();
    const Parameter* parameter = parameterAt(graph, target.address);
    if (!parameter) return;

    const float span = target.high - target.low;
    if (std::fabs(span) < 1.0e-6f) return;

    control->value = clampValue((parameter->normalised() - target.low) / span, 0.0f, 1.0f);
}

void Surface::adoptAllFromGraph(const Graph& graph) {
    const Page* current = page(activePage_);
    if (!current) return;

    // Collected first, because adoptFromGraph looks the control up again and
    // iterating the vector while calling into something that does is asking for
    // trouble the first time either side grows a resize.
    std::vector<int> ids;
    ids.reserve(current->controls.size());
    for (const Control& control : current->controls) ids.push_back(control.id);

    for (const int id : ids) adoptFromGraph(id, graph);
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

bool Surface::bind(int id, ParamAddress address, const Graph& graph, bool secondAxis) {
    Control* control = find(id);
    if (!control || !control->drivesParameters() || !address.valid()) return false;
    if (secondAxis && !control->hasSecondAxis()) return false;

    const Parameter* parameter = parameterAt(graph, address);
    if (!parameter) return false;

    std::vector<Target>& targets = secondAxis ? control->targetsY : control->targets;
    for (const Target& existing : targets)
        if (existing.address == address) return false;   // already bound

    // The parameter's current value becomes the end of the range the control is
    // nearest. Binding with the knob down means "this is what down sounds like",
    // and the performer then turns it up and dials in the top - which is how
    // anyone actually builds a macro, rather than by typing two numbers.
    const float amount = secondAxis ? control->valueY : control->value;
    const float here = parameter->normalised();

    Target target;
    target.address = address;
    if (amount < 0.5f) { target.low = here; target.high = 1.0f; }
    else               { target.low = 0.0f; target.high = here; }

    targets.push_back(target);
    return true;
}

bool Surface::unbind(int id, ParamAddress address, bool secondAxis) {
    Control* control = find(id);
    if (!control) return false;

    std::vector<Target>& targets = secondAxis ? control->targetsY : control->targets;
    const auto it = std::find_if(targets.begin(), targets.end(),
                                 [address](const Target& t) { return t.address == address; });
    if (it == targets.end()) return false;

    targets.erase(it);
    return true;
}

bool Surface::setTargetRange(int id, ParamAddress address, float low, float high,
                             bool secondAxis) {
    Control* control = find(id);
    if (!control) return false;

    std::vector<Target>& targets = secondAxis ? control->targetsY : control->targets;
    for (Target& target : targets) {
        if (!(target.address == address)) continue;
        // Not sorted or separated: `low` above `high` is an inverted target,
        // which is a thing to want rather than a mistake to correct.
        target.low = clampValue(low, 0.0f, 1.0f);
        target.high = clampValue(high, 0.0f, 1.0f);
        return true;
    }
    return false;
}

void Surface::pruneMissing(const Graph& graph) {
    const auto prune = [&graph](std::vector<Target>& targets) {
        targets.erase(std::remove_if(targets.begin(), targets.end(),
                                     [&graph](const Target& t) {
                                         return parameterAt(graph, t.address) == nullptr;
                                     }),
                      targets.end());
    };

    for (Page& current : pages_) {
        for (Control& control : current.controls) {
            prune(control.targets);
            prune(control.targetsY);
        }
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

JsonValue Surface::toJson() const {
    JsonValue root = JsonValue::object();
    root.set("columns", columns_);
    root.set("rows", rows_);
    root.set("activePage", activePage_);

    JsonValue pageArray = JsonValue::array();
    for (const Page& current : pages_) {
        JsonValue pageObject = JsonValue::object();
        pageObject.set("name", current.name);

        JsonValue controlArray = JsonValue::array();
        for (const Control& control : current.controls) {
            JsonValue entry = JsonValue::object();
            entry.set("id", control.id);
            entry.set("kind", toString(control.kind));
            entry.set("name", control.name);
            entry.set("colour", static_cast<int>(control.colour));
            entry.set("column", control.column);
            entry.set("row", control.row);
            entry.set("width", control.width);
            entry.set("height", control.height);
            entry.set("value", control.value);
            entry.set("valueY", control.valueY);
            entry.set("momentary", control.momentary);
            entry.set("targets", targetsToJson(control.targets));
            if (control.hasSecondAxis())
                entry.set("targetsY", targetsToJson(control.targetsY));
            controlArray.push(std::move(entry));
        }

        pageObject.set("controls", std::move(controlArray));
        pageArray.push(std::move(pageObject));
    }

    root.set("pages", std::move(pageArray));
    return root;
}

void Surface::fromJson(const JsonValue& in) {
    clear();
    if (!in.isObject()) return;

    setGrid(in.getInt("columns", 12), in.getInt("rows", 8));

    const JsonValue* pageArrayPtr = in.find("pages");
    if (pageArrayPtr != nullptr && pageArrayPtr->isArray() && pageArrayPtr->size() > 0) {
        const JsonValue& pageArray = *pageArrayPtr;
        pages_.clear();

        for (std::size_t p = 0; p < pageArray.size(); ++p) {
            const JsonValue& pageObject = pageArray.at(p);
            if (!pageObject.isObject()) continue;

            Page current;
            current.name = pageObject.getString("name", "page");

            const JsonValue* controlPtr = pageObject.find("controls");
            const JsonValue controlArray = controlPtr ? *controlPtr : JsonValue::array();
            for (std::size_t c = 0; controlArray.isArray() && c < controlArray.size(); ++c) {
                const JsonValue& entry = controlArray.at(c);
                if (!entry.isObject()) continue;

                Control control;
                control.id = entry.getInt("id", 0);
                control.kind = controlKindFromString(entry.getString("kind", "knob"));
                control.name = entry.getString("name");
                control.colour = static_cast<std::uint32_t>(entry.getInt("colour", 0));
                control.column = entry.getInt("column", 0);
                control.row = entry.getInt("row", 0);
                control.width = std::max(1, entry.getInt("width", 2));
                control.height = std::max(1, entry.getInt("height", 2));
                control.value = entry.getFloat("value", 0.0f);
                control.valueY = entry.getFloat("valueY", 0.0f);
                control.momentary = entry.getBool("momentary", false);
                if (const JsonValue* t = entry.find("targets"))
                    control.targets = targetsFromJson(*t);
                if (const JsonValue* t = entry.find("targetsY"))
                    control.targetsY = targetsFromJson(*t);

                // Ids come from the file, so the counter has to clear the
                // highest of them or the next control added collides with one
                // that is already there and `find` returns the wrong one.
                nextId_ = std::max(nextId_, control.id + 1);
                current.controls.push_back(std::move(control));
            }

            pages_.push_back(std::move(current));
        }
    }

    if (pages_.empty()) pages_.push_back(Page{ "main", {} });
    activePage_ = clampValue(in.getInt("activePage", 0), 0, pageCount() - 1);
}

void Surface::clear() {
    pages_.clear();
    pages_.push_back(Page{ "main", {} });
    activePage_ = 0;
    nextId_ = 1;
    columns_ = 12;
    rows_ = 8;
}

} // namespace acm::control
