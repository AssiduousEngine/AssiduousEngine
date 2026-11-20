using Engine;

// The game: the title, the waves, the score and the end of a run.
//
// The title comes up first with the yard held still behind it. Play starts the first wave: after a
// short lull the wave's enemies come in through the three gaps, a few seconds apart (brawlers, then
// gunners from the second wave and the heavy hazmat suits from the fourth), and once every one of
// them is down the next wave follows, bigger and quicker. The run ends when the player falls
// or the last wave is cleared; the end screen shows the score and the best (saved), and plays again
// or goes back to the title. The HUD shows the wave, the score, the player's health, the weapon and
// its rounds, and a hit marker. Pause (Escape, Start, or a pointer lock the browser ended) holds the
// yard behind the pause menu: resume, restart or back to the title.
// Behaviours reach the game through its statics (one assembly, one game).
public class Game : RunBehavior
{
    const string TitleDocument = "{201c1f62-4f2e-4809-8ab9-f56ae5eb677f}";
    const string HudDocument = "{2f377ba9-ba65-48b4-87e6-4fba2e1cea35}";
    const string EndDocument = "{d9ca4ad9-598b-45cb-a39c-bf3bffbf3224}";
    const string PauseDocument = "{719602b7-527b-4711-a159-a1da49d3694d}";
    const string EnemyPrefab = "d219b0bc-1afb-4279-9f1b-805041793de9";
    const string GunnerPrefab = "43b797b1-f59e-43ed-a994-74fd9f89d8b0";
    const string HazmatPrefab = "9e063c7c-3142-451c-bddd-beee9ecbac67";
    const string YardScene = "bd9364eb-553f-4ad6-860d-1964af5da10e";

    const int Waves = 6;
    const string BestKey = "best";

    // Where enemies come in: just outside the north, east and west gaps.
    static readonly Float3[] Gaps =
    {
        new Float3 { x = 0.0f, y = 0.0f, z = -17.5f },
        new Float3 { x = 21.0f, y = 0.0f, z = -4.0f },
        new Float3 { x = -21.0f, y = 0.0f, z = 6.0f },
    };

    static Game? s_current;

    /// <summary>Whether a run is being played: the player moves and fires only then.</summary>
    public static bool Playing => s_current != null && s_current.m_playing && !s_current.m_paused;

    /// <summary>How much tougher this wave's enemies are than the first's: 1 on wave 1.</summary>
    public static float Difficulty => s_current == null ? 1.0f : 1.0f + 0.12f * (s_current.m_wave - 1);

    bool m_playing;
    bool m_paused;
    bool m_wasLocked; // the pointer was locked last frame: a lock lost since pauses
    bool m_held;      // a menu holds the yard still (Hold)
    int m_wave;
    int m_toSpawn;    // this wave's enemies not yet in
    readonly System.Collections.Generic.List<string> m_queue = new(); // their prefabs, in the order they come
    readonly System.Random m_random = new();
    int m_alive;      // enemies in and not down
    float m_nextSpawn;
    float m_lull;
    int m_score;
    float m_noteLeft;

    float m_hitShown;
    float m_hurtShown;

    ScreenStack? Screens => ScreenStack.of(gui.root()?.find("screens"));

    public void onStart()
    {
        s_current = this;
        ShowTitle();
    }

    public void onStop()
    {
        if (s_current == this)
            s_current = null;
    }

    public void onUpdate(float dt)
    {
        UpdateHud(dt);
        if (!m_playing)
            return;
        UpdatePause();
        if (m_paused)
            return;
        if (m_lull > 0.0f)
        {
            m_lull -= dt;
            return;
        }
        if (m_toSpawn > 0)
        {
            m_nextSpawn -= dt;
            if (m_nextSpawn <= 0.0f)
            {
                SpawnEnemy();
                m_nextSpawn = System.MathF.Max(0.6f, 1.6f - 0.15f * m_wave);
            }
            return;
        }
        if (m_alive <= 0)
        {
            if (m_wave >= Waves)
                End(true);
            else
                StartWave(m_wave + 1);
        }
    }

    // ---- the title and the end ----

    void ShowTitle()
    {
        m_playing = false;
        m_paused = false;
        Hold(true);
        Screens?.clear();
        Screens?.push(TitleDocument);
        SetText("title-best", BestLine());
        On("title-play", Play);
        On("title-quit", () => run.requestExit());
    }

    void Play()
    {
        Screens?.clear();
        Screens?.push(HudDocument);
        m_score = 0;
        m_alive = 0;
        m_paused = false;
        m_wasLocked = false;
        SetText("hud-score", "0");
        SetText("hud-health", "100");
        Hold(false);
        m_playing = true;
        StartWave(1);
    }

    void End(bool held)
    {
        m_playing = false;
        int best = Save.getInt(BestKey, 0);
        bool record = m_score > best;
        if (record)
        {
            Save.setInt(BestKey, m_score);
            Save.flush();
        }
        Hold(true);
        Sounds.PlayUI(held ? Sounds.Held : Sounds.Overrun);
        Screens?.push(EndDocument);
        SetText("end-heading", held ? "The yard is held" : "Overrun");
        SetText("end-waves", held ? $"All {Waves} waves held off" : $"Fell on wave {m_wave} of {Waves}");
        SetText("end-score", $"{m_score}");
        SetText("end-best", record ? "A new best" : $"Best {best}");
        On("end-again", Restart);
        On("end-title", () => { Reload(); ShowTitle(); });
    }

    // ---- pause ----

    // Pause holds the yard behind its menu; pressed again it carries on. A lock the browser ended
    // (its Escape) pauses too, so the game never runs with the pointer loose.
    void UpdatePause()
    {
        bool locked = Input.pointerLocked();
        bool lost = m_wasLocked && !locked;
        m_wasLocked = locked;
        if (m_paused)
        {
            if (Input.wasPressed("Pause"))
                Resume();
            return;
        }
        if (lost || Input.wasPressed("Pause"))
            Pause();
    }

    void Pause()
    {
        m_paused = true;
        Hold(true);
        Screens?.push(PauseDocument);
        On("pause-resume", Resume);
        On("pause-restart", Restart);
        On("pause-title", () => { Reload(); ShowTitle(); });
    }

    void Resume()
    {
        m_paused = false;
        m_wasLocked = false;
        Screens?.pop();
        Hold(false);
    }

    void Restart()
    {
        Reload();
        Play();
    }

    // The yard as it was: the run's scene loaded again, the fallen enemies and the player's health
    // with it.
    void Reload()
    {
        Engine.Guid yard = Engine.Guid.Parse(YardScene);
        if (!yard.IsNil())
            run.loadScene(yard);
    }

    // Behind a menu the yard holds still, the pointer is free and the player's actions read nothing.
    void Hold(bool held)
    {
        if (held == m_held)
            return;
        m_held = held;
        run.setTimeScale(held ? 0.0f : 1.0f);
        Input.lockPointer(!held);
        if (held)
            Input.pushSet("Menu");
        else
            Input.popSet();
    }

    // ---- the waves ----

    void StartWave(int wave)
    {
        m_wave = wave;
        // Brawlers every wave, gunners from the second, the heavy hazmat suits from the fourth, in a
        // shuffled order.
        m_queue.Clear();
        int gunners = wave / 2;
        int heavies = System.Math.Max(0, wave - 3);
        for (int i = 0; i < 2 + wave; ++i)
            m_queue.Add(EnemyPrefab);
        for (int i = 0; i < gunners; ++i)
            m_queue.Add(GunnerPrefab);
        for (int i = 0; i < heavies; ++i)
            m_queue.Add(HazmatPrefab);
        for (int i = m_queue.Count - 1; i > 0; --i)
        {
            int j = m_random.Next(i + 1);
            (m_queue[i], m_queue[j]) = (m_queue[j], m_queue[i]);
        }
        m_toSpawn = m_queue.Count;
        m_nextSpawn = 0.0f;
        m_lull = wave == 1 ? 2.0f : 4.0f;
        SetText("hud-wave", $"{wave}");
        Sounds.PlayUI(Sounds.Wave);
        Note(wave == Waves ? "Last wave" : $"Wave {wave}");
    }

    void SpawnEnemy()
    {
        Engine.Guid prefab = Engine.Guid.Parse(m_queue[m_toSpawn - 1]);
        Float3 gap = Gaps[m_toSpawn % Gaps.Length];
        // Spread a little sideways, so two from the same gap do not stand in one spot.
        float spread = (m_toSpawn % 3 - 1) * 1.2f;
        Float3 at = new Float3 { x = gap.x + (gap.z > -10.0f ? 0.0f : spread), y = gap.y, z = gap.z + (gap.z > -10.0f ? spread : 0.0f) };
        Entity spawned = run.currentScene().prefabs.spawn(prefab, at);
        if (!spawned.isValid())
        {
            Log.warn("C# Game: an enemy did not spawn");
            m_toSpawn = 0;
            return;
        }
        m_toSpawn -= 1;
        m_alive += 1;
    }

    // ---- what behaviours tell the game ----

    /// <summary>An enemy went down: the score goes up by the wave's worth, and the count shown.</summary>
    public static void EnemyDown()
    {
        if (s_current == null)
            return;
        Game game = s_current;
        game.m_alive = System.Math.Max(0, game.m_alive - 1);
        game.m_score += 100 * game.m_wave;
        game.SetText("hud-score", $"{game.m_score}");
    }

    /// <summary>The player was hurt: the edges flash red, the health shown falls to `health`, and at
    /// none the run is over.</summary>
    public static void ShowDamage(float health)
    {
        if (s_current == null)
            return;
        Game game = s_current;
        game.m_hurtShown = 0.4f;
        game.SetText("hud-health", $"{System.MathF.Ceiling(health)}");
        if (health <= 0.0f && game.m_playing)
            game.End(false);
    }

    /// <summary>The player's health shown, without the flash of a hurt (a health pack).</summary>
    public static void ShowHealth(float health) => s_current?.SetText("hud-health", $"{System.MathF.Ceiling(health)}");

    /// <summary>The weapon in hand and its rounds ("12 / 90"), or what it is doing ("Reloading").</summary>
    public static void ShowWeapon(string name, string rounds)
    {
        if (s_current == null)
            return;
        s_current.SetText("hud-weapon", name.ToUpperInvariant()); // as the HUD's other captions
        s_current.SetText("hud-ammo", rounds);
    }

    /// <summary>A shot landed: the marker round the crosshair shows and fades.</summary>
    public static void ShowHit()
    {
        if (s_current == null)
            return;
        s_current.m_hitShown = 0.15f;
        Sounds.Play(Sounds.Hit, 0.5f);
        if (Find("hud-hit") is Node marker)
            marker.opacity = 1.0f;
    }

    // ---- the HUD ----

    void UpdateHud(float dt)
    {
        if (m_hitShown > 0.0f && Find("hud-hit") is Node marker)
        {
            m_hitShown -= dt;
            marker.opacity = System.MathF.Max(0.0f, m_hitShown / 0.15f);
        }
        if (m_hurtShown > 0.0f && Find("hud-hurt") is Node hurt)
        {
            m_hurtShown -= dt;
            hurt.opacity = System.MathF.Max(0.0f, m_hurtShown / 0.4f) * 0.6f;
        }
        if (m_noteLeft > 0.0f)
        {
            m_noteLeft -= dt;
            if (m_noteLeft <= 0.0f)
                SetText("hud-note", "");
        }
    }

    void Note(string text)
    {
        SetText("hud-note", text);
        m_noteLeft = 2.5f;
    }

    string BestLine()
    {
        int best = Save.getInt(BestKey, 0);
        return best > 0 ? $"Best {best}" : "";
    }

    static Node? Find(string id) => gui.root()?.find(id);

    void SetText(string id, string text)
    {
        if (Label.of(Find(id)) is Label label)
            label.text = text;
    }

    static void On(string id, System.Action action)
    {
        if (Find(id) is Node node)
            node.on("click", _ => { Sounds.PlayUI(Sounds.Click); action(); return true; });
    }
}
