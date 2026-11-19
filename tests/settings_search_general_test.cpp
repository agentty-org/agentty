// settings_search_general_test — #133: the hub search must find the General
// hub's own rows (toggles like "Keep retrying on errors"), not only the
// fields of the form panes, and opening such a hit lands on that row.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"   // settings_search_index
#include "agentty/runtime/panel/settings/categories.hpp"
#include "agentty/runtime/panel/settings/items.hpp"

#include <string>

namespace pn = agentty::ui::panel;
namespace se = agentty::settings;
using namespace agentty;

TEST_CASE("settings search: every General action row is indexed") {
    Model m;
    const auto rows = se::items_for(m, se::Category::General);
    const auto idx  = app::detail::settings_search_index(m);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& r = rows[static_cast<std::size_t>(i)];
        if (r.action == se::Action::None) continue;
        bool found = false;
        for (const auto& h : idx)
            if (h.pane == se::ListOpen::Pane::General && h.item == i
                && h.label == r.primary) found = true;
        INFO(r.primary);
        CHECK(found);
    }
}

TEST_CASE("settings search: \"retry\" finds and opens Keep retrying on errors") {
    Model m;
    m = app::update(std::move(m),
                    Msg{OpenSettingsList{se::Category::General}}).first;
    REQUIRE(m.ui.panel.is<pn::SettingsList>());
    for (char32_t c : std::u32string{U"retry"})
        m = app::update(std::move(m), Msg{SettingsSearchInput{c}}).first;

    const auto* o = m.ui.panel.get<pn::SettingsList>();
    REQUIRE(o);
    const auto hits = se::search(o->index_rows, o->query);
    REQUIRE(!hits.empty());
    const auto& top = o->index_rows[static_cast<std::size_t>(hits.front())];
    CHECK(top.label == "Keep retrying on errors");
    CHECK(top.pane == se::ListOpen::Pane::General);

    m = app::update(std::move(m), Msg{SettingsListActivate{}}).first;
    const auto* hub = m.ui.panel.get<pn::SettingsList>();
    REQUIRE(hub);
    CHECK(hub->concern == se::Category::General);
    CHECK(hub->query.empty());
    const auto rows = se::items_for(m, se::Category::General);
    REQUIRE(hub->index >= 0);
    REQUIRE(hub->index < static_cast<int>(rows.size()));
    CHECK(rows[static_cast<std::size_t>(hub->index)].primary
          == "Keep retrying on errors");

    // Esc returns to the search, query intact.
    m = app::update(std::move(m), Msg{CloseSettingsList{}}).first;
    const auto* back = m.ui.panel.get<pn::SettingsList>();
    REQUIRE(back);
    CHECK(back->query == "retry");
}
