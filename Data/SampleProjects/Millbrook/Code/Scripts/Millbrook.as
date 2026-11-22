// Millbrook: the game (the reserved class Game). It puts up the HUD, and while no conversation
// plays it watches who the player is near: the villager in reach (kTalkReach) is named on the HUD's
// talk prompt, and Interact talks with them (talk). The conversations themselves are the dialogue
// plugin's; the villagers are found in the square by their model's name, as the cast names them.

const string kHudDoc = "{c73aadcb-3022-4f3f-8adf-97c454da32be}";
const float kTalkReach = 2.6f; // how near the player must be to talk with someone (m)

// The villagers: the entity each is in the square (its model's name), and what the prompt calls them.
array<string> kVillagers = {"character-male-e", "character-female-b", "character-male-c", "character-female-d",
                            "character-knight"};
array<string> kCalled = {"the mayor", "the baker", "the miller", "Pip", "the guard"};

ScreenStack@ screens() { return ScreenStack::of(gui::root().find("screens")); }

Node@ find(string id)
{
    Screen@ top = screens().top();
    Node@ n = top !is null ? top.find(id) : null;
    return n !is null ? n : gui::root().find(id);
}

class Game : RunBehavior
{
    private int m_near = -1; // the villager the prompt names; -1 none

    void onStart()
    {
        screens().push(kHudDoc);
    }

    void onUpdate(float dt)
    {
        Scene scene = run::currentScene();
        Entity player = scene.find("Player");
        if (!player.isValid())
        {
            return;
        }
        int near = -1;
        if (!Dialogue::isPlaying())
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
        if (m_near >= 0 && Input::wasPressed("Interact"))
        {
            talk(m_near, scene, player);
        }
    }

    // Talks with villager `who`: the player turns to them. (The conversations come with the cast.)
    private void talk(int who, Scene scene, Entity player)
    {
        Entity villager = scene.find(kVillagers[who]);
        player.send("FaceTo", villager.worldPosition());
        Log::info("talk with " + kCalled[who]);
    }
}
