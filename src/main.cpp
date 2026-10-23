// AI Level Builder — a Geode mod for Geometry Dash.
// Adds an "AI" button to the level editor. You describe a level, the mod
// asks an AI (Gemini, Claude, OpenAI or any OpenAI-compatible server) for a
// list of objects, then places them in your level.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

using namespace geode::prelude;

// ─────────────────────────────────────────────────────────────────────────────
// Object palette: the names the AI is allowed to use, mapped to GD object IDs.
// ─────────────────────────────────────────────────────────────────────────────
static const std::unordered_map<std::string, int> kObjectIds = {
    {"block",          1},
    {"spike",          8},
    {"half_spike",     39},
    {"small_spike",    103},
    {"yellow_pad",     35},
    {"pink_pad",       140},
    {"yellow_orb",     36},
    {"pink_orb",       141},
    {"blue_orb",       84},
    {"gravity_down",   10},
    {"gravity_up",     11},
    {"cube_portal",    12},
    {"ship_portal",    13},
    {"ball_portal",    47},
    {"ufo_portal",     111},
    {"wave_portal",    660},
    {"speed_slow",     200},
    {"speed_normal",   201},
    {"speed_fast",     202},
};

// One grid cell in the GD editor is 30 units. Row 0 sits on the ground.
static constexpr float kCell    = 30.f;
static constexpr float kGroundY = 105.f;

// Objects placed by the last AI build, so it can be undone with one tap.
static std::vector<Ref<GameObject>> s_lastBuild;

static std::string buildSystemPrompt(int maxObjects) {
    std::string names;
    for (auto& [name, id] : kObjectIds) {
        if (!names.empty()) names += ", ";
        names += name;
    }
    return fmt::format(
R"(You design Geometry Dash levels. Reply with ONLY a JSON object, no explanation, no markdown:
{{"objects":[{{"t":"block","x":0,"y":0}},{{"t":"spike","x":6,"y":0,"rot":0}}]}}

Coordinates are grid cells. x goes right (the player moves right). y=0 is the row sitting on the ground, y=1 is one block above it.
"rot" is optional rotation in degrees (use 180 for an upside-down spike on a ceiling).
Allowed "t" values: {}.

Rules for a playable level (cube at normal speed):
- The cube jumps about 2 blocks high and about 4 blocks far.
- Never put more than 3 spikes in a row on the ground.
- Leave at least 3 empty cells between separate obstacles.
- Platforms (rows of blocks) can float; the player must be able to jump onto them.
- Pads and orbs launch the player; put them before a gap or a tall wall.
- Portals are 3 blocks tall; place them with y=1 so they sit above the ground.
- Start the first obstacle at x >= 8. Keep everything at x >= 0 and y >= 0.
- Use between 40 and {} objects. Make the level fit what the player asked for.)",
        names, maxObjects);
}

// Pulls the text out of each provider's response format.
static Result<std::string> extractText(std::string const& provider, matjson::Value const& json) {
    if (json.contains("error")) {
        auto const& err = json["error"];
        std::string msg = err.isObject() && err.contains("message")
            ? err["message"].asString().unwrapOr("unknown error")
            : err.dump();
        return Err("AI error: " + msg);
    }
    if (provider == "gemini") {
        if (!json.contains("candidates") || !json["candidates"].isArray() || json["candidates"].size() == 0)
            return Err("Gemini returned no answer (maybe blocked by its safety filter)");
        auto const& cand = json["candidates"][0];
        if (!cand.contains("content") || !cand["content"].contains("parts"))
            return Err("Gemini returned an empty answer");
        auto const& parts = cand["content"]["parts"];
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) {
            auto const& p = parts[i];
            if (p.contains("thought") && p["thought"].asBool().unwrapOr(false)) continue;
            if (p.contains("text")) out += p["text"].asString().unwrapOr("");
        }
        return Ok(out);
    }
    if (provider == "claude") {
        if (!json.contains("content") || !json["content"].isArray())
            return Err("Claude returned an empty answer");
        std::string out;
        auto const& blocks = json["content"];
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i].contains("text")) out += blocks[i]["text"].asString().unwrapOr("");
        }
        return Ok(out);
    }
    // openai + custom (OpenAI-compatible)
    if (!json.contains("choices") || !json["choices"].isArray() || json["choices"].size() == 0)
        return Err("The AI returned no answer");
    auto const& msg = json["choices"][0]["message"];
    if (!msg.contains("content")) return Err("The AI returned an empty answer");
    return Ok(msg["content"].asString().unwrapOr(""));
}

static double num(matjson::Value const& obj, char const* key, double fallback = 0.0) {
    if (!obj.contains(key)) return fallback;
    auto const& v = obj[key];
    if (v.isNumber()) return v.asDouble().unwrapOr(fallback);
    if (v.isString()) {
        auto s = v.asString().unwrapOr("");
        char* end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        if (end != s.c_str()) return d;
    }
    return fallback;
}

// ─────────────────────────────────────────────────────────────────────────────
// The popup where you type your level idea.
// ─────────────────────────────────────────────────────────────────────────────
class AIBuildPopup : public Popup {
protected:
    LevelEditorLayer* m_editor = nullptr;
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_buildBtn = nullptr;
    async::TaskHolder<web::WebResponse> m_task;
    bool m_busy = false;
    std::string m_provider;

    bool init(LevelEditorLayer* editor) {
        m_editor = editor;
        constexpr float W = 380.f, H = 220.f;
        if (!Popup::init(W, H)) return false;

        this->setID("ai-build-popup"_spr);
        this->setTitle("AI Level Builder");

        m_input = TextInput::create(330.f, "Describe your level...", "chatFont.fnt");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(400);
        m_input->setPosition({W / 2.f, H - 75.f});
        m_mainLayer->addChild(m_input);

        m_status = CCLabelBMFont::create("e.g. \"easy ship section with orbs\"", "chatFont.fnt");
        m_status->setScale(0.7f);
        m_status->setPosition({W / 2.f, H - 115.f});
        m_mainLayer->addChild(m_status);

        auto menu = CCMenu::create();
        menu->setPosition({W / 2.f, 40.f});
        m_mainLayer->addChild(menu);

        m_buildBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Build", "goldFont.fnt", "GJ_button_01.png", 0.9f),
            this, menu_selector(AIBuildPopup::onBuild));
        m_buildBtn->setPositionX(-70.f);
        menu->addChild(m_buildBtn);

        auto undoBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Undo AI", "goldFont.fnt", "GJ_button_06.png", 0.8f),
            this, menu_selector(AIBuildPopup::onUndo));
        undoBtn->setPositionX(70.f);
        menu->addChild(undoBtn);

        return true;
    }

    void setStatus(std::string const& text, ccColor3B color = {255, 255, 255}) {
        // Keep long error messages readable on a phone screen.
        std::string shown = text.size() > 70 ? text.substr(0, 67) + "..." : text;
        m_status->setString(shown.c_str());
        m_status->setColor(color);
        m_status->limitLabelWidth(340.f, 0.7f, 0.3f);
    }

    void onUndo(CCObject*) {
        if (s_lastBuild.empty()) {
            setStatus("Nothing to undo.", {255, 200, 0});
            return;
        }
        int removed = 0;
        for (auto& obj : s_lastBuild) {
            if (obj && obj->getParent()) {
                m_editor->removeObject(obj, true);
                ++removed;
            }
        }
        s_lastBuild.clear();
        if (m_editor->m_editorUI) m_editor->m_editorUI->updateButtons();
        setStatus(fmt::format("Removed {} AI objects.", removed), {0, 255, 100});
    }

    void onBuild(CCObject*) {
        if (m_busy) return;
        std::string prompt = std::string(m_input->getString());
        if (prompt.empty()) {
            setStatus("Type what level you want first!", {255, 200, 0});
            return;
        }

        auto mod = Mod::get();
        m_provider = mod->getSettingValue<std::string>("provider");
        std::string key = mod->getSettingValue<std::string>("api-key");
        std::string model = mod->getSettingValue<std::string>("model");
        int maxObjects = static_cast<int>(mod->getSettingValue<int64_t>("max-objects"));

        if (key.empty() && m_provider != "custom") {
            setStatus("Add your API key in the mod settings first.", {255, 80, 80});
            return;
        }

        std::string system = buildSystemPrompt(maxObjects);
        std::string userMsg = "Build this level: " + prompt;

        auto req = web::WebRequest();
        req.header("Content-Type", "application/json");
        req.timeout(std::chrono::seconds(120));
        std::string url;
        auto body = matjson::Value::object();

        if (m_provider == "gemini") {
            if (model.empty()) model = "gemini-3.6-flash";
            url = fmt::format("https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model);
            req.header("x-goog-api-key", key);

            auto sysPart = matjson::Value::object();
            sysPart["text"] = system;
            auto sysParts = matjson::Value::array();
            sysParts.push(sysPart);
            auto sysInstr = matjson::Value::object();
            sysInstr["parts"] = sysParts;
            body["systemInstruction"] = sysInstr;

            auto userPart = matjson::Value::object();
            userPart["text"] = userMsg;
            auto userParts = matjson::Value::array();
            userParts.push(userPart);
            auto content = matjson::Value::object();
            content["role"] = "user";
            content["parts"] = userParts;
            auto contents = matjson::Value::array();
            contents.push(content);
            body["contents"] = contents;

            auto gen = matjson::Value::object();
            gen["responseMimeType"] = "application/json";
            body["generationConfig"] = gen;
        }
        else if (m_provider == "claude") {
            if (model.empty()) model = "claude-sonnet-5-5";
            url = "https://api.anthropic.com/v1/messages";
            req.header("x-api-key", key);
            req.header("anthropic-version", "2023-06-01");

            body["model"] = model;
            body["max_tokens"] = 16000;
            body["system"] = system;
            auto msg = matjson::Value::object();
            msg["role"] = "user";
            msg["content"] = userMsg;
            auto msgs = matjson::Value::array();
            msgs.push(msg);
            body["messages"] = msgs;
        }
        else {
            // openai or custom OpenAI-compatible server
            if (m_provider == "openai") {
                url = "https://api.openai.com/v1/chat/completions";
                if (model.empty()) model = "gpt-4o";
            } else {
                url = mod->getSettingValue<std::string>("custom-url");
                if (model.empty()) {
                    setStatus("Set a model name in the settings for custom.", {255, 80, 80});
                    return;
                }
            }
            if (!key.empty()) req.header("Authorization", "Bearer " + key);

            body["model"] = model;
            auto sys = matjson::Value::object();
            sys["role"] = "system";
            sys["content"] = system;
            auto user = matjson::Value::object();
            user["role"] = "user";
            user["content"] = userMsg;
            auto msgs = matjson::Value::array();
            msgs.push(sys);
            msgs.push(user);
            body["messages"] = msgs;
        }

        req.bodyString(body.dump());

        m_busy = true;
        m_buildBtn->setEnabled(false);
        setStatus("Asking the AI... (this can take a minute)", {120, 200, 255});
        log::info("AI Level Builder: sending request to {} ({})", m_provider, model);

        m_task.spawn(req.post(url), [this](web::WebResponse resp) {
            m_busy = false;
            m_buildBtn->setEnabled(true);
            this->onResponse(resp);
        });
    }

    void onResponse(web::WebResponse& resp) {
        auto text = resp.string().unwrapOr("");
        if (!resp.ok()) {
            log::error("AI Level Builder: HTTP {}: {}", resp.code(), text);
            std::string hint;
            if (resp.code() == 401 || resp.code() == 403) hint = " (check your API key)";
            else if (resp.code() == 404) hint = " (check the model name)";
            else if (resp.code() == 429) hint = " (rate limit, wait a bit)";
            else if (resp.code() <= 0) hint = " (no internet?)";
            setStatus(fmt::format("Request failed: HTTP {}{}", resp.code(), hint), {255, 80, 80});
            return;
        }

        auto json = matjson::parse(text);
        if (!json) {
            setStatus("Couldn't read the AI's reply.", {255, 80, 80});
            return;
        }
        auto answer = extractText(m_provider, json.unwrap());
        if (!answer) {
            setStatus(answer.unwrapErr(), {255, 80, 80});
            return;
        }
        this->placeLevel(answer.unwrap());
    }

    void placeLevel(std::string answer) {
        // The AI sometimes wraps JSON in ```json fences or adds text around it:
        // keep only the outermost {...}.
        auto first = answer.find('{');
        auto last = answer.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first) {
            log::error("AI Level Builder: no JSON in answer: {}", answer);
            setStatus("The AI didn't send a level. Try again.", {255, 80, 80});
            return;
        }
        auto parsed = matjson::parse(std::string_view(answer).substr(first, last - first + 1));
        if (!parsed) {
            log::error("AI Level Builder: bad JSON: {}", answer);
            setStatus("The AI's level was broken JSON. Try again.", {255, 80, 80});
            return;
        }
        auto root = parsed.unwrap();
        if (!root.contains("objects") || !root["objects"].isArray()) {
            setStatus("The AI's reply had no objects. Try again.", {255, 80, 80});
            return;
        }
        auto const& objects = root["objects"];

        // Where to start building.
        float startX = 0.f;
        if (Mod::get()->getSettingValue<std::string>("place-at") == "after-last-object"
            && m_editor->m_objects)
        {
            float maxX = -1.f;
            for (unsigned i = 0; i < m_editor->m_objects->count(); ++i) {
                auto obj = static_cast<GameObject*>(m_editor->m_objects->objectAtIndex(i));
                if (obj) maxX = std::max(maxX, obj->getPositionX());
            }
            if (maxX >= 0.f) startX = maxX + kCell * 3.f;
        }
        // Snap to the grid.
        startX = std::floor(startX / kCell) * kCell;

        int maxObjects = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-objects"));
        s_lastBuild.clear();
        int placed = 0, skipped = 0;

        for (size_t i = 0; i < objects.size() && placed < maxObjects; ++i) {
            auto const& o = objects[i];
            if (!o.isObject()) { ++skipped; continue; }

            int id = 0;
            std::string type;
            if (o.contains("t")) type = o["t"].asString().unwrapOr("");
            else if (o.contains("type")) type = o["type"].asString().unwrapOr("");
            if (!type.empty()) {
                auto it = kObjectIds.find(type);
                if (it != kObjectIds.end()) id = it->second;
            }
            if (id == 0 && o.contains("id")) id = static_cast<int>(num(o, "id"));
            if (id <= 0) { ++skipped; continue; }

            double gx = std::max(0.0, num(o, "x"));
            double gy = std::clamp(num(o, "y"), 0.0, 80.0);
            CCPoint pos = {
                startX + static_cast<float>(gx) * kCell + kCell / 2.f,
                kGroundY + static_cast<float>(gy) * kCell
            };

            auto obj = m_editor->createObject(id, pos, false);
            if (!obj) { ++skipped; continue; }
            double rot = num(o, "rot");
            if (rot != 0.0) obj->setRotation(static_cast<float>(rot));
            s_lastBuild.emplace_back(obj);
            ++placed;
        }

        if (m_editor->m_editorUI) m_editor->m_editorUI->updateButtons();
        log::info("AI Level Builder: placed {} objects, skipped {}", placed, skipped);

        if (placed == 0) {
            setStatus("The AI's level was empty. Try again.", {255, 80, 80});
            return;
        }
        Notification::create(
            fmt::format("AI built {} objects!", placed), NotificationIcon::Success)->show();
        // Close on the next frame, not from inside the network callback.
        geode::queueInMainThread([self = Ref<AIBuildPopup>(this)] {
            self->onClose(nullptr);
        });
    }

public:
    static AIBuildPopup* create(LevelEditorLayer* editor) {
        auto ret = new AIBuildPopup();
        if (ret->init(editor)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Adds the "AI" button to the editor.
// ─────────────────────────────────────────────────────────────────────────────
class $modify(AIEditorUI, EditorUI) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

        // A new editor means the old build's objects are gone.
        s_lastBuild.clear();

        auto spr = ButtonSprite::create("AI", 30, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AIEditorUI::onAIButton));
        btn->setID("ai-button"_spr);

        if (auto menu = this->getChildByID("editor-buttons-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            // Fallback: our own little menu near the top-right.
            auto winSize = CCDirector::get()->getWinSize();
            auto menu2 = CCMenu::create();
            menu2->setID("ai-menu"_spr);
            menu2->setPosition({winSize.width - 30.f, winSize.height - 90.f});
            menu2->addChild(btn);
            this->addChild(menu2);
        }
        return true;
    }

    void onAIButton(CCObject*) {
        if (auto popup = AIBuildPopup::create(m_editorLayer)) popup->show();
    }
};
