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
// Each game is a save slot (three: "1", "2", "3"), the saves demo: the title's Continue opens the
// slot saved last, New game the first empty one (or asks which to replace), Load lists them with how
// far each fair has come and when it was saved. The slot opened is handed to the dialogue
// (Dialogue::useSave), which keeps its flags and a conversation's place in it: each conversation's
// end saves, and a game that starts with a conversation in progress resumes it. The global store
// keeps what every game shares: how many fairs have opened, on the credits.
// The game opens on the title over the village. When the
// mayor has called everyone (the flag opening), the ending plays (Sequences/FairOpens: its camera,
// the lanterns lit on its LightLanterns event, its lines in the stock box), then the credits; the
// flag fair_opened keeps the lanterns lit after. Pause (Escape or Start) holds the village still
// under the pause menu: Resume, Leave to the title, Quit.

const string kHudDoc = "{{{GUI:Hud}}}";
const string kTitleDoc = "{{{GUI:Title}}}";
const string kCreditsDoc = "{{{GUI:Credits}}}";
const string kPauseDoc = "{{{GUI:Pause}}}";
const Guid kFairOpens = Guid("{{Sequence:FairOpens}}"); // the ending
const int kLanterns = 4; // LanternLight0..3 (village.py)
const Guid kFairTune = Guid("{{Audio:FairTune}}"); // the fair's music, from the start
const float kTalkReach = 2.6f;   // how near the player must be to talk with someone (m)
const float kWalkSpeed = 1.6f;   // a villager's walk (m/s)
const float kCatReach = 5.0f;    // how near the windmill the player sees the cat (m)
const int kSlots = 3;            // the saves the title offers, slots "1" to "3"
const string kFairsKey = "fairs.opened"; // the global store's count of fairs opened, over every game

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

funcdef bool EventHandler(GuiEvent e);
void wire(string id, EventHandler@ handler) { find(id).on("click", handler); } // a button's click

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
    private bool m_playing = false;  // past the title, the credits not up
    private int m_ending = 0;        // the ending's play id (a notice's play); 0 none
    private SequencePlay m_endingPlay; // held on its last frame (black) behind the credits
    private bool m_paused = false;
    private SaveStore m_slot;         // the game in play, its slot open; invalid at the title
    private bool m_replacing = false; // the slots list picks a game to replace (New game, all used)
    // Where the village begins, read as the run starts: what a session moves (the villagers' walks,
    // the player's errand, the cart's roll), which a new game puts back.
    private array<string> m_placed;
    private array<Float3> m_startPositions;
    private array<Quaternion> m_startRotations;

    void onStart()
    {
        rememberStart();
        Audio::playMusic(kFairTune);
        idleAll();
        lightLanterns(false); // no game yet: the title's village is the one every game begins in
        showTitle();
    }

    // ---- the title and the saves ----
    private void showTitle()
    {
        m_playing = false;
        run::setTimeScale(0.0f); // the village holds still behind the title
        screens().clear();
        screens().push(kTitleDoc);
        array<string>@ saved = Saves::slots();
        const bool any = saved.length() > 0 && Saves::slotTime(saved[0]) > 0; // saved last first
        if (!any)
        {
            UIWidget::of(find("continue-btn")).addClass("gone"); // nothing to go on with
        }
        if (saved.length() == 0)
        {
            UIWidget::of(find("load-btn")).addClass("gone");
        }
        wire("continue-btn", EventHandler(this.onContinue));
        wire("new-btn", EventHandler(this.onNewGame));
        wire("load-btn", EventHandler(this.onLoad));
        wire("quit-btn", EventHandler(this.onQuit));
        wire("slots-back", EventHandler(this.onSlotsBack));
        for (int n = 1; n <= kSlots; n++)
        {
            wire("slot-" + n, EventHandler(this.onSlot));
            wire("slot-" + n + "-delete", EventHandler(this.onSlotDelete));
        }
    }

    private bool onContinue(GuiEvent e)
    {
        array<string>@ saved = Saves::slots();
        if (saved.length() > 0)
        {
            play(saved[0]);
        }
        return true;
    }

    // A new game in the first empty slot; with all of them used, the list asks which to replace.
    private bool onNewGame(GuiEvent e)
    {
        for (int n = 1; n <= kSlots; n++)
        {
            if (!Saves::hasSlot("" + n))
            {
                play("" + n);
                return true;
            }
        }
        showSlots(true);
        return true;
    }

    private bool onLoad(GuiEvent e)
    {
        showSlots(false);
        return true;
    }

    private bool onSlotsBack(GuiEvent e)
    {
        UIWidget::of(find("slots-card")).addClass("gone");
        UIWidget::of(find("title-card")).removeClass("gone");
        return true;
    }

    // The list of the three, each with its game's summary, read without opening it.
    private void showSlots(bool replacing)
    {
        m_replacing = replacing;
        Label::of(find("slots-head")).text = replacing ? "All three are in use: which game gives way?" : "Load a game";
        for (int n = 1; n <= kSlots; n++)
        {
            const string name = "" + n;
            Button::of(find("slot-" + n)).text = "Game " + n + ": " + summary(name);
            UIWidget@ remove = UIWidget::of(find("slot-" + n + "-delete"));
            if (Saves::hasSlot(name) && !replacing)
            {
                remove.removeClass("gone");
            }
            else
            {
                remove.addClass("gone");
            }
        }
        UIWidget::of(find("title-card")).addClass("gone");
        UIWidget::of(find("slots-card")).removeClass("gone");
    }

    // How far a game's fair has come (its dialogue flags) and when it was saved; "empty" for none.
    private string summary(const string &in name)
    {
        if (!Saves::hasSlot(name))
        {
            return "empty";
        }
        SaveStore game = Saves::readSlot(name);
        string stage = "just arrived";
        if (game.getBool("flag.fair_opened", false))
        {
            stage = "the fair is open";
        }
        else if (game.getBool("flag.fair_ready", false))
        {
            stage = "the fair is ready";
        }
        else if (game.getBool("flag.met_mayor", false))
        {
            stage = "readying the fair";
        }
        return stage + ", " + saved(Saves::slotTime(name));
    }

    // "saved 2 hours ago", or the date once it is days ago.
    private string saved(double at)
    {
        if (at <= 0.0)
        {
            return "not saved yet";
        }
        const double ago = Time::unixNow() - at;
        if (ago < 60.0)
        {
            return "saved just now";
        }
        if (ago < 3600.0)
        {
            return "saved " + int(ago / 60.0) + " min ago";
        }
        if (ago < 86400.0)
        {
            const int hours = int(ago / 3600.0);
            return "saved " + hours + (hours == 1 ? " hour ago" : " hours ago");
        }
        return "saved " + Text::formatDate(at, "DD/MM/YYYY");
    }

    private bool onSlot(GuiEvent e)
    {
        const string name = slotOf(e);
        if (name.isEmpty())
        {
            return true;
        }
        if (m_replacing)
        {
            Saves::deleteSlot(name); // a new game in its place
        }
        else if (!Saves::hasSlot(name))
        {
            return true; // an empty slot loads nothing; New game makes one
        }
        play(name);
        return true;
    }

    private bool onSlotDelete(GuiEvent e)
    {
        const string name = slotOf(e);
        if (!name.isEmpty())
        {
            Saves::deleteSlot(name);
            showSlots(false);
        }
        return true;
    }

    // The slot a row's button names, by its id ("slot-2", "slot-2-delete": "2").
    private string slotOf(GuiEvent e)
    {
        for (int n = 1; n <= kSlots; n++)
        {
            if (e.id == "slot-" + n || e.id == "slot-" + n + "-delete")
            {
                return "" + n;
            }
        }
        return "";
    }

    // Opens the game `name` (made when new) and plays it: the dialogue keeps its flags in it, the
    // village is put back as every game begins, and its lanterns lit if its fair has opened.
    private void play(const string &in name)
    {
        m_slot = Saves::openSlot(name);
        if (!m_slot.isValid())
        {
            return;
        }
        Dialogue::useSave(m_slot);
        putBack();
        lightLanterns(Dialogue::flag("fair_opened"));
        begin();
    }

    private void rememberStart()
    {
        Scene scene = run::currentScene();
        array<string> names = kVillagers;
        names.insertLast("Player");
        names.insertLast("Cart");
        for (uint i = 0; i < names.length(); i++)
        {
            Entity placed = scene.find(names[i]);
            if (placed.isValid())
            {
                m_placed.insertLast(names[i]);
                m_startPositions.insertLast(placed.worldPosition());
                m_startRotations.insertLast(placed.rotation());
            }
        }
    }

    private void putBack()
    {
        Scene scene = run::currentScene();
        for (uint i = 0; i < m_placed.length(); i++)
        {
            Entity placed = scene.find(m_placed[i]);
            if (!placed.isValid())
            {
                continue;
            }
            placed.setRotation(m_startRotations[i]);
            CharacterComponent@ character = CharacterComponent::of(placed);
            if (character !is null)
            {
                character.setPosition(m_startPositions[i]); // the player walks as a character
            }
            else
            {
                placed.setPosition(m_startPositions[i]);
            }
        }
        m_walker = -1;
        idleAll();
    }

    private void begin()
    {
        screens().clear();
        screens().push(kHudDoc);
        m_near = -1;
        run::setTimeScale(1.0f);
        m_playing = true;
        m_talkedLastFrame = true; // the press that began is not a talk
        // A save made mid conversation goes on where it was.
        Dialogue::resume();
    }

    private bool onQuit(GuiEvent e) { run::requestExit(0); return true; }

    private void showCredits()
    {
        m_playing = false;
        screens().clear();
        screens().push(kCreditsDoc);
        const int fairs = Saves::global().getInt(kFairsKey, 1);
        Label::of(find("credits-fairs")).text = fairs == 1 ? "The first fair opened here" : "Fairs opened here: " + fairs;
        wire("back-btn", EventHandler(this.onBackToVillage));
    }

    private bool onBackToVillage(GuiEvent e)
    {
        m_endingPlay.stop(); // the view and the light come back
        screens().clear();
        screens().push(kHudDoc);
        m_near = -1;
        m_playing = true;
        m_talkedLastFrame = true;
        return true;
    }

    // ---- the pause menu ----
    // The village holds still, and the input map's Menu set (Pause alone) is the one in effect, so
    // the player's actions read released under the menu and the press that clicks a button is
    // never read as a talk. A conversation's box stays, waiting, under the menu.
    private void pause()
    {
        m_paused = true;
        run::setTimeScale(0.0f);
        Input::pushSet("Menu");
        screens().push(kPauseDoc);
        wire("resume-btn", EventHandler(this.onResume));
        wire("title-btn", EventHandler(this.onLeaveToTitle));
        wire("quit-btn", EventHandler(this.onQuit));
    }

    private bool onResume(GuiEvent e)
    {
        resume();
        return true;
    }

    private void resume()
    {
        if (!m_paused)
        {
            return;
        }
        m_paused = false;
        screens().pop();
        Input::popSet();
        run::setTimeScale(1.0f);
        m_talkedLastFrame = true; // a press that closed the menu is not a talk
    }

    // Back to the title: a conversation under way ends (its flags so far are saved), and an ending
    // under way stops, to play again when the mayor is next asked.
    private bool onLeaveToTitle(GuiEvent e)
    {
        resume();
        if (Dialogue::isPlaying())
        {
            Dialogue::stop();
        }
        if (m_ending != 0)
        {
            m_endingPlay.stop();
            m_ending = 0;
            run::currentScene().find("Player").send("Hold", false);
        }
        m_walker = -1;
        idleAll(); // a villager stopped mid-walk stands
        Saves::closeSlot(); // written, then let go: the title has no game open
        m_slot = SaveStore();
        showTitle();
        return true;
    }

    // ---- the ending ----
    private void playEnding(Scene scene)
    {
        m_near = -1;
        UIWidget::of(find("hud-talk")).removeClass("shown");
        scene.find("Player").send("Hold", true);
        // held at its end, so the credits come up over black; Back to the village lets it go. Made,
        // its camera bound, then started, so its first frame is the camera's.
        m_endingPlay = scene.sequences.make(kFairOpens, "hold");
        m_endingPlay.bind("EndingCamera", scene.find("EndingCamera")); // a play fills its parts itself
        m_endingPlay.start();
        m_ending = int(m_endingPlay.id());
    }

    void onSequenceEvent(SequenceNotice@ notice)
    {
        if (notice.play == m_ending && notice.name == "LightLanterns")
        {
            lightLanterns(true);
        }
    }

    void onSequenceFinished(SequenceNotice@ notice)
    {
        if (notice.play != m_ending)
        {
            return;
        }
        m_ending = 0;
        Dialogue::setFlag("fair_opened", true);
        SaveStore shared = Saves::global();
        shared.setInt(kFairsKey, shared.getInt(kFairsKey, 0) + 1); // over every game
        Saves::flush();
        run::currentScene().find("Player").send("Hold", false);
        showCredits();
    }

    private void lightLanterns(bool lit)
    {
        Scene scene = run::currentScene();
        for (int i = 0; i < kLanterns; i++)
        {
            Entity light = scene.find("LanternLight" + i);
            if (light.isValid())
            {
                LightComponent::of(light).enabled = lit;
            }
        }
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
        if (!player.isValid() || !m_playing)
        {
            return;
        }
        if (Input::wasPressed("Pause"))
        {
            if (m_paused)
            {
                resume();
            }
            else
            {
                pause();
            }
            return;
        }
        if (m_paused)
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
        if (!Dialogue::isPlaying() && m_walker < 0 && m_ending == 0)
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
        if (notice.conversation.isEmpty())
        {
            return; // a line the ending's sequence showed, not a conversation
        }
        Saves::flush();
        if (Dialogue::flag("opening") && !Dialogue::flag("fair_opened"))
        {
            playEnding(run::currentScene());
            return;
        }
        if (Dialogue::flag("fair_ready") && !Dialogue::flag("fair_opened"))
        {
            note("The fair is ready to open.");
        }
    }
}
