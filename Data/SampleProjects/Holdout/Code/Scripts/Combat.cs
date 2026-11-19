using System.Collections.Generic;
using Engine;

// What can be hurt: a behaviour that implements IDamageable and registers its entity here as it
// starts. A shot or a punch finds what it struck through the registry, from the entity hit or any
// entity under it (an enemy's body collider), and calls it directly with the damage and where it
// landed: one assembly, so no message is needed.
//
// Statics live as long as the assembly, which outlasts a round (Play again reloads the yard) and,
// in the editor, a play session: so an entry whose entity is gone is dropped rather than trusted.
public interface IDamageable
{
    void OnDamage(Float3 at, float amount);
}

public static class Combat
{
    static readonly List<(Entity Entity, IDamageable Target)> s_damageable = new();

    // The player's body, which the enemies chase; invalid until the player starts.
    public static Entity Player;

    public static void Register(Entity entity, IDamageable target)
    {
        Forget(entity);
        s_damageable.Add((entity, target));
    }

    public static void Unregister(Entity entity) => Forget(entity);

    static void Forget(Entity entity) =>
        s_damageable.RemoveAll(entry => !entry.Entity.isValid() || Entity.Equals(entry.Entity, entity));

    // Everything that can be hurt within `radius` of `at`, with its entity (a blast's reach). A copy:
    // hurting one may unregister it, or another.
    public static List<(Entity Entity, IDamageable Target)> Near(Float3 at, float radius)
    {
        var near = new List<(Entity, IDamageable)>();
        foreach ((Entity entity, IDamageable target) in s_damageable)
            if (entity.isValid() && Float3.Distance(entity.worldPosition(), at) <= radius)
                near.Add((entity, target));
        return near;
    }

    // What `entity`, or the nearest entity above it, is if it can be hurt; null when nothing is.
    public static IDamageable? Find(Entity entity)
    {
        for (Entity at = entity; at.isValid(); at = at.parent())
            foreach ((Entity registered, IDamageable target) in s_damageable)
                if (registered.isValid() && Entity.Equals(registered, at))
                    return target;
        return null;
    }
}
