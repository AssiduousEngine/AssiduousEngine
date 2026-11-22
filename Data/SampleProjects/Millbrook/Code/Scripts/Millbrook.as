// Millbrook: the game (the reserved class Game). It puts up the HUD, and while no conversation
// plays it watches who the player is near: the villager in reach (kTalkReach) is named on the HUD's
// talk prompt, and Interact plays their conversation, the two turned to face each other. The
// conversations are the dialogue plugin's (Tools/dialogue/*.dialogue), shown in its stock box; this
// script carries out the commands they declare and keeps what the plugin cannot know:
//   * walk <who> to <where> [and wait]: the speaker walks there, playing their walk clip; a
//     conversation told to wait goes on when they arrive (Dialogue::done).
//   * errand <where>: the conversation leaves with its place kept; the player and the miller go
//     round to the waterwheel, and the conversation resumes there (Dialogue::resume).
//   * fair_cat_seen: once Pip has asked, reaching the windmill's foot shows her cat there (a flag
//     the game provides; the settings list fair_* as the game's).
// The save keeps every flag and a conversation's place: each conversation's end saves, and a game
// that starts with a conversation in progress resumes it.

const string kHudDoc = "{c73aadcb-3022-4f3f-8adf-97c454da32be}";
const Guid kFairTune = Guid("8754cba6-29ef-4d5f-8b1b-7193cf8df3e6"); // the fair's music, from the start
const float kTalkReach = 2.6f;   // how near the player must be to talk with someone (m)
const float kWalkSpeed = 1.6f;   // a villager's walk (m/s)
const float kCatReach = 5.0f;    // how near the windmill the player sees the cat (m)

// The villagers: the entity each is in the square (its model's name), what the prompt calls them,
// and the conversation that starts when the player talks with them, in its dialogue file.
array<string> kVillagers = {"character-male-e", "character-female-b", "character-male-c", "character-female-d",
                            "character-knight"};
array<string> kCalled = {"the mayor", "the baker", "the miller", "Pip", "the guard"};
array<string> kSpeakers = {"mayor", "baker", "miller", "pip", "guard"};
array<string> kConversations = {"mayor", "baker", "miller", "pip", "guard"};
array<Guid> kFiles = {Guid("7a97915a-e9eb-476d-ba58-5dd5fb4ed885"), Guid("cfba85cf-a545-40f5-8ed6-ec85e82916f6"), Guid("69beea3e-381e-4b30-be8c-2e6476a3d77a"),
                      Guid("7a97915a-e9eb-476d-ba58-5dd5fb4ed885"), Guid("7a97915a-e9eb-476d-ba58-5dd5fb4ed885")};
// Each villager's walk and idle clips (their own model's).
array<Guid> kWalkClips = {Guid("9b6fb9c1-4a44-4fed-86c5-79951aa1d54e"), Guid("655956d7-2cd2-4c9d-8d74-df9f5b928311"),
                          Guid("255ee2c4-a55f-4c52-8acb-b845f902503c"), Guid("f7e2add8-da89-4277-9cda-fc94b1bfdb4b"),
                          Guid("d88364a3-e102-43af-9815-8cf8f77acd4c")};
array<Guid> kIdleClips = {Guid("85d75eee-7333-42ee-9e10-d8faf46c25f1"), Guid("217c7044-fec1-48f0-871d-57eb1f6bf49d"),
                          Guid("15e42897-dff0-4970-816b-9db503eabb16"), Guid("483dcaa5-14f0-4a76-9a7f-4f7c424c4f38"),
                          Guid("a2d3b14e-7583-4192-b59e-b93f23745366")};

ScreenStack@ screens() { return ScreenStack::of(gui::root().find("screens")); }

Node@ find(string id)
{
    Screen@ top = screens().top();
    Node@ n = top !is null ? top.find(id) : null;
    return n !is null ? n : gui::root().find(id);
}

// The villager speaking as `speaker`, by its index; -1 for the player or no one.
int villagerOf(const string &in speaker)
{
    for (uint i = 0; i < kSpeakers.length(); i++)
    {
        if (kSpeakers[i] == speaker)
        {
            return int(i);
        }
    }
    return -1;
}

// Turns `who` to face the point `at` on the ground.
void faceTo(Entity who, Float3 at)
{
    Float3 here = who.worldPosition();
    who.setRotationEuler(0.0f, Math::RadiansToDegrees(Math::Atan2(at.x - here.x, at.z - here.z)), 0.0f);
}

class Game : RunBehavior
{
    private int m_near = -1;      // the villager the prompt names; -1 none
    private int m_walker = -1;    // the villager a walk command moves; -1 none
    private Float3 m_walkTo;
    private bool m_walkWaits = false;
    private float m_noteFor = 0.0f;
    private bool m_talkedLastFrame = false; // a conversation played last frame: its last press is not a new talk

    void onStart()
    {
        screens().push(kHudDoc);
        Audio::playMusic(kFairTune);
        idleAll();
        // A save made mid conversation goes on where it was.
        Dialogue::resume();
    }

    private void idleAll()
    {
        Scene scene = run::currentScene();
        for (uint i = 0; i < kVillagers.length(); i++)
        {
            Entity villager = scene.find(kVillagers[i]);
            if (villager.isValid())
            {
                scene.animation.setClip(villager, kIdleClips[i], 0.0f);
            }
        }
    }

    void onUpdate(float dt)
    {
        Scene scene = run::currentScene();
        Entity player = scene.find("Player");
        if (!player.isValid())
        {
            return;
        }
        walking(scene, dt);
        lookForCat(scene, player);
        if (m_noteFor > 0.0f)
        {
            m_noteFor -= dt;
            if (m_noteFor <= 0.0f)
            {
                UIWidget::of(find("hud-note")).removeClass("shown");
            }
        }
        int near = -1;
        if (!Dialogue::isPlaying() && m_walker < 0)
        {
            Float3 at = player.worldPosition();
            float best = kTalkReach;
            for (uint i = 0; i < kVillagers.length(); i++)
            {
                Entity villager = scene.find(kVillagers[i]);
                if (!villager.isValid())
                {
                    continue;
                }
                Float3 there = villager.worldPosition();
                float dx = there.x - at.x;
                float dz = there.z - at.z;
                float distance = Math::Sqrt(dx * dx + dz * dz);
                if (distance < best)
                {
                    best = distance;
                    near = int(i);
                }
            }
        }
        if (near != m_near)
        {
            m_near = near;
            UIWidget@ prompt = UIWidget::of(find("hud-talk"));
            if (near >= 0)
            {
                Label::of(find("hud-talk-text")).text = "Talk with " + kCalled[near];
                prompt.addClass("shown");
            }
            else
            {
                prompt.removeClass("shown");
            }
        }
        // The press that ends a conversation is the box's, not a new talk.
        if (m_near >= 0 && !m_talkedLastFrame && Input::wasPressed("Interact"))
        {
            talk(m_near, scene, player);
        }
        m_talkedLastFrame = Dialogue::isPlaying();
    }

    // Plays villager `who`'s conversation, the two turned to each other.
    private void talk(int who, Scene scene, Entity player)
    {
        Entity villager = scene.find(kVillagers[who]);
        faceTo(villager, player.worldPosition());
        player.send("FaceTo", villager.worldPosition());
        Dialogue::play(kFiles[who], kConversations[who]);
    }

    // A walk under way: the villager steps toward the place, then stands, and a conversation that
    // waits for them goes on.
    private void walking(Scene scene, float dt)
    {
        if (m_walker < 0)
        {
            return;
        }
        Entity villager = scene.find(kVillagers[m_walker]);
        Float3 at = villager.worldPosition();
        float dx = m_walkTo.x - at.x;
        float dz = m_walkTo.z - at.z;
        float left = Math::Sqrt(dx * dx + dz * dz);
        float step = kWalkSpeed * dt;
        if (left <= step)
        {
            villager.setPosition(Float3(m_walkTo.x, at.y, m_walkTo.z));
            scene.animation.setClip(villager, kIdleClips[m_walker], 0.2f);
            Entity player = scene.find("Player");
            faceTo(villager, player.worldPosition());
            m_walker = -1;
            if (m_walkWaits)
            {
                Dialogue::done();
            }
            return;
        }
        villager.setPosition(Float3(at.x + dx / left * step, at.y, at.z + dz / left * step));
        faceTo(villager, m_walkTo);
    }

    // Once Pip has asked, the windmill's foot shows her cat: the flag the conversations read.
    private void lookForCat(Scene scene, Entity player)
    {
        if (!Dialogue::flag("asked_cat") || Dialogue::flag("fair_cat_seen") || Dialogue::flag("cat_found"))
        {
            return;
        }
        Entity spot = scene.find("WindmillSpot");
        Float3 a = player.worldPosition();
        Float3 b = spot.worldPosition();
        float dx = b.x - a.x;
        float dz = b.z - a.z;
        if (dx * dx + dz * dz < kCatReach * kCatReach)
        {
            Dialogue::setFlag("fair_cat_seen", true);
            note("A small grey cat watches you from the windmill's step.");
        }
    }

    private void note(const string &in text)
    {
        Label::of(find("hud-note")).text = text;
        UIWidget::of(find("hud-note")).addClass("shown");
        m_noteFor = 4.0f;
    }

    // The commands the conversations declare (the plugin's settings): walk and errand.
    void onDialogueCommand(DialogueCommandNotice@ notice)
    {
        Scene scene = run::currentScene();
        if (notice.name == "walk")
        {
            int who = villagerOf(notice.text("who"));
            Entity place = notice.entity("where");
            if (who < 0 || !place.isValid())
            {
                if (notice.waits)
                {
                    Dialogue::done();
                }
                return;
            }
            m_walker = who;
            m_walkTo = place.worldPosition();
            m_walkWaits = notice.waits;
            scene.animation.setClip(scene.find(kVillagers[who]), kWalkClips[who], 0.2f);
        }
        else if (notice.name == "errand")
        {
            // The conversation has left, its place kept: round the back together, then on with it.
            Entity place = notice.entity("where");
            Entity player = scene.find("Player");
            Entity miller = scene.find("character-male-c");
            Entity millerSpot = scene.find("MillerAtWheel");
            if (place.isValid() && player.isValid())
            {
                Float3 at = place.worldPosition();
                CharacterComponent::of(player).setPosition(Float3(at.x, at.y + 0.85f, at.z));
                if (miller.isValid() && millerSpot.isValid())
                {
                    miller.setPosition(millerSpot.worldPosition());
                    faceTo(miller, at);
                    player.send("FaceTo", millerSpot.worldPosition());
                }
            }
            Dialogue::resume();
        }
    }

    // Each conversation's end saves the flags (and a place kept by an errand).
    void onDialogueEnded(DialogueEndedNotice@ notice)
    {
        Save::flush();
        if (Dialogue::flag("fair_ready"))
        {
            note("The fair is ready to open.");
        }
    }
}
