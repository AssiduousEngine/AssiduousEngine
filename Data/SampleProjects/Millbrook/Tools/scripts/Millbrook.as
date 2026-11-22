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

const string kHudDoc = "{{{GUI:Hud}}}";
const Guid kFairTune = Guid("{{Audio:FairTune}}"); // the fair's music, from the start
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
array<Guid> kFiles = {Guid("{{Dialogue:Square}}"), Guid("{{Dialogue:Bakery}}"), Guid("{{Dialogue:Mill}}"),
                      Guid("{{Dialogue:Square}}"), Guid("{{Dialogue:Square}}")};
// Each villager's walk and idle clips (their own model's).
array<Guid> kWalkClips = {Guid("{{AnimationClip:character-male-e/walk}}"), Guid("{{AnimationClip:character-female-b/walk}}"),
                          Guid("{{AnimationClip:character-male-c/walk}}"), Guid("{{AnimationClip:character-female-d/walk}}"),
                          Guid("{{AnimationClip:character-knight/walk}}")};
array<Guid> kIdleClips = {Guid("{{AnimationClip:character-male-e/idle}}"), Guid("{{AnimationClip:character-female-b/idle}}"),
                          Guid("{{AnimationClip:character-male-c/idle}}"), Guid("{{AnimationClip:character-female-d/idle}}"),
                          Guid("{{AnimationClip:character-knight/idle}}")};

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
