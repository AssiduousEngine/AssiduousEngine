# RHI handles

> STATUS: APPROVED 2026-11-22. Sized XL, in phases H0 to H6, green after each.
> Origin: the user asked whether handles would make the RHI's API better or safer (2026-11-22);
> the answer was yes, for lifetime, and a survey of the engine found the other places a slot is
> reused without a generation. Read CONVENTIONS.md first.

## Goal

An RHI resource is named by a typed handle (a slot and a generation), never by a pointer the
caller keeps. A handle used after its resource was destroyed fails the lookup, in every build,
instead of reaching freed memory; a destroyed resource stays alive on the device until the GPU can
no longer be using it, without its owner arranging that; and caches keyed on a resource cannot be
fooled by a new resource at an old address.

The engine gets one generational handle and pool in Core, which the RHI uses first and the other
slot-reusing ids (physics joints and characters, navigation agents) move onto after.

Not goals: changing what the RHI can do, its backends' internals beyond the object tables, the
render graph's own handles (they are already generational), and reference counting.

## What exists (checked 2026-11-22)

- **Resources are pointers to backend objects.** `Device::CreateBuffer(const BufferDesc&,
  Buffer*& out)` and its siblings (`Code/Foundation/RHI/Device.cppm:201`) hand out a pointer to an
  object a backend derives from the interface's class (`VkBufferImpl : public Buffer`,
  `RHI.Vulkan/VkBuffer.cppm:25`); `DestroyBuffer(Buffer*&)` frees it and nulls the caller's copy
  only (`Device.cppm:224`). The resource classes are in `RHI/Resources.cppm`: Buffer, Texture,
  TextureView, Sampler, ShaderModule, Surface, CommandBuffer, Fence, QuerySet, BindGroupLayout,
  BindGroup, PipelineLayout, PipelineCache, RenderPipeline, ComputePipeline, MeshPipeline,
  AccelStruct, RayTracingPipeline. Some carry methods (`Buffer::Map`, `Unmap`; `BindGroup::Layout`)
  and public fields (`Buffer::desc`, `Texture::desc`).
- **Encoders take pointers**: `SetBindGroup(u32, BindGroup*, ...)`, `SetVertexBuffer(u32, Buffer*,
  u64)` (`RHI/Commands.cppm:38`, `:45`).
- **Frames in flight are each owner's concern.** `GpuRetireQueue` (`Render/Resources.cppm`) keeps a
  retired buffer, texture, view or bind group until the frames that might use it have aged out,
  ticked once a frame; it is wired into the render subsystem's systems, and a consumer without it
  falls back to `WaitIdle` and destroys at once (`RetireBindGroup`, `Render/BindGroupCache.cppm:67`;
  `MeshRenderer::RetireOrDrainBuffer`). On the web, a `WaitIdle` mid-frame drops the frame, which
  is why the queue exists. Retiring appears in 47 source files.
- **Caches key on addresses.** `BindGroupCache` keys on a `TextureView*` and keeps a group only if
  every input matches (`Render/BindGroupCache.cppm:119`): a freed view's address reused by a new
  one would otherwise hand back a group built for the old one. That was the class of the player's
  TAA stale bind group.
- **Validation tracks what is alive for leaks** (`RHI.Validation/ValidatedDevice.cppm`), but its
  encoders do not check that what they are given is still alive.
- **Reach**: the RHI's types appear in about 90 modules (the renderer, materials, textures,
  shaders, the GUI and VG renderers, terrain, particles, the editor's previews, the samples), and
  1129 lines name `rhi::Buffer*`, `Texture*`, `TextureView*` or `BindGroup*`. The backends are
  Vulkan, Direct3D 12, WebGPU (desktop and browser) and Null, under the Validation and Capture
  layers.
- **Generational handles elsewhere, each written by hand**: `EntityHandle` (`Scene/Entity.cppm`),
  `RGHandle` (`RenderGraph/Types.cppm`), `VoiceHandle` (`Audio/AudioEngine.cppm`). `BodyId` keeps
  Jolt's index and sequence number.
- **Slots reused without a generation**: `JointId` and `CharacterId`
  (`Physics/PhysicsWorld.cppm`; a freed slot is taken by the next joint or character,
  `PhysicsWorldImpl.cpp:1330`, `:1631`), and navigation agents, Detour crowd indices as `i32`
  (`Navigation/NavigationMesh.cppm:160`).

## Prior art (read 2026-11-22)

- **ezEngine** (`RendererFoundation`): a handle type per resource (`ezGALBufferHandle`, an 18-bit
  index and a 14-bit generation, `RendererFoundationDLL.h:623`) over id tables of the backend's
  objects (`ezIdTable`, `Device/Device.h:312`). `DestroyBuffer(ezGALBufferHandle&)` puts the object
  on the device's dead list (`m_DeadObjects`), freed later by `DestroyDeadObjects`, and the backend
  implements `DestroyBufferPlatform`.
- **Traktor** (`Render/IRenderSystem.h:102`): `Ref<Buffer> createBuffer(...)`, reference counted.
- **LumixEngine** (`renderer/gpu/gpu.h:18`): `using BufferHandle = Buffer*`, a pointer by another
  name.
- **Flax** (`Graphics/GPUDevice.h:511`): `GPUBuffer* CreateBuffer(...)`, objects by pointer.

What to take: ezEngine's shape (typed handles, the backends' objects in tables, destruction
deferred by the device). Reference counting (Traktor) also prevents use after free, but by keeping
an object alive while anything refers to it: a forgotten reference leaks GPU memory quietly, every
copy pays an atomic count, and frames in flight still need deferring. Handles keep ownership
explicit and only add a check.

## Decision 1 - one generational handle and pool, in Core

- **`Handle<Tag>`**: a slot index and a generation, trivially copyable, comparable and hashable,
  with an invalid value; the tag makes a buffer's handle a different type from a texture's.
- **`HandlePool<Tag, T>`**: `Allocate(T) -> Handle`, `Get(Handle) -> T*` (null when the handle is
  invalid or its generation is not the slot's), `Free(Handle)` (bumps the slot's generation, so
  every copy of the handle goes stale at once), iteration over what is live. Storage is in fixed
  pages, so a lookup never sees storage move while another thread allocates.
- **A pool stamps its id into the handles it makes**: the generation's top 8 bits are the pool's
  id, its low 24 the slot's count. A handle from one pool fails the generation compare in any other
  pool of its type, in every build and at no cost, so a buffer of one device used on another, or a
  joint of one scene's physics world used in another's, is caught as a stale handle is. A slot
  whose 24-bit count would wrap (16,777,216 reuses) is retired rather than reused.
- The owner's allocator is threaded, as everywhere.
- `RGHandle` and `VoiceHandle` move onto it in H6. `EntityHandle` stays the scene's own (user,
  2026-11-22); whether it moves later is in `Plans/backlog-0.2.md`.

## Decision 2 - which RHI objects are handles

- **Handles**: what other resources or commands refer to, and what a cache can key on: Buffer,
  Texture, TextureView, Sampler, ShaderModule, BindGroupLayout, BindGroup, PipelineLayout,
  PipelineCache, RenderPipeline, ComputePipeline, MeshPipeline, QuerySet, AccelStruct,
  RayTracingPipeline.
- **Objects, as now**: what a caller drives by its methods and nothing else refers to: the
  device, queue, surface, swap chain, command pool, encoders, command buffer and fence. They stay
  pure interfaces, created and destroyed as today.

## Decision 3 - the API

```cpp
namespace foundation::rhi
{
    using BufferHandle = core::Handle<struct BufferTag>;
    using TextureHandle = core::Handle<struct TextureTag>;
    // ... one per type of Decision 2

    class Device
    {
    public:
        virtual Status CreateBuffer(const BufferDesc& desc, BufferHandle& out) = 0;
        virtual void DestroyBuffer(BufferHandle& buffer) = 0; // the handle is stale at once; the
                                                              // object is freed when the GPU is done
        [[nodiscard]] virtual const BufferDesc* Describe(BufferHandle buffer) const = 0; // null: stale
        [[nodiscard]] virtual void* Map(BufferHandle buffer) = 0;
        virtual void Unmap(BufferHandle buffer) = 0;
        // ... and so for each type
    };

    class RenderPassEncoder
    {
    public:
        virtual void SetVertexBuffer(u32 slot, BufferHandle buffer, u64 offset = 0) = 0;
        virtual void SetBindGroup(u32 index, BindGroupHandle group, ...) = 0;
        // ...
    };
}
```

- **What was a method or field on a resource is a device call** taking its handle (`Map`, `Unmap`,
  `Describe`, a bind group's layout). Descriptors that name resources (`BindGroupDesc`'s entries, a
  render pass's attachments) hold handles.
- **The interface stays pure virtual** and every backend implements every call.
- **A stale handle**: a lookup fails. The call does nothing and returns its failure (a `Status`, a
  null, a skipped command), and the device logs it once per handle with the label the resource was
  created with, in every build. The Validation layer turns it into an error with the call that
  made it.

## Decision 4 - the device owns the tables and frames in flight

- **Each backend's device keeps a `HandlePool` per type** of its own objects (the Vulkan device's
  pool holds `VkBufferImpl`), so a lookup reaches the backend's object with no cast, and each
  device's pools carry its id (Decision 1), so a handle used on the wrong device fails its lookup.
  One table shared by the backends was weighed and not taken: it would catch the same mistake, but
  at the cost of a cast on every lookup and one lock across devices (user, 2026-11-22).
- **`Destroy*(handle)` makes the handle stale at once and retires the object**, and the device
  frees it once the GPU can no longer use it. A command buffer recorded before the destroy may be
  submitted after it (a frame's commands recorded, an upload that destroys a buffer submitted, then
  the frame's commands), so "every submission made before the destroy" is not enough (user,
  2026-11-22): a retired object waits until **every command buffer begun before the destroy has been
  submitted or discarded**, and then until the last of those submissions completes. A shared tracker
  in `foundation.rhi` does the bookkeeping; each backend reports a command buffer's begin, its
  submit (with the queue's submission value) and its discard, and its queue signals an internal
  timeline on every submit (a timeline semaphore on Vulkan, a fence on Direct3D 12) whose completed
  value the tracker compares. WebGPU's
  own objects already outlive the work that uses them; its backend frees at once if that holds in
  the browser and wgpu-native, and retires as the others do if it does not (checked in H1).
- **`GpuRetireQueue`, the per-pass `Retire*` paths and their `WaitIdle` fallbacks go**: a pass that
  replaces a resource destroys the old one and is done. The web frame drop the queue was written
  for cannot happen, since the device never waits.
- **Shutdown** frees everything retired after the device's final idle.

## Decision 5 - caches key on handles

`BindGroupCache` and every other map keyed on an RHI pointer keys on handles. A handle names one
resource for good (its generation changes when the resource goes), so a cached group whose inputs
were destroyed is never handed back for new ones; the compare-every-input guard stays only where an
input can change without being destroyed.

## Decision 6 - debugging

A handle shows as a slot and a generation in a debugger, not the object. `Device::Label(handle)`
gives the label the resource was created with, and the device's log of a stale use names it. A
pretty printer for gdb and lldb, and a natvis for Visual Studio, show a handle's label and whether
it is live, through the device the process has.

## Decision 7 - the other ids

`JointId` and `CharacterId` become Core handles over the physics world's slots, so a joint or
character id kept past its removal no longer reaches the next one in its slot. Navigation agents
get a handle the crowd's wrapper maps to Detour's index, for the same reason. `BodyId` stays Jolt's
(its sequence number is a generation already).

## Phases

- **H0 - Core handles.** `Handle<Tag>` and `HandlePool<Tag, T>` in Core with their tests: stale
  after free, every copy stale, a handle from another pool fails, the count's wrap retires the slot,
  pages never move, iteration, allocator threaded.
- **H1 - buffers, end to end.** `BufferHandle` through the interface, Null, Vulkan, Direct3D 12,
  WebGPU, Validation and Capture, and every caller; the device's retirement by submission for
  buffers; WebGPU's lifetime checked in the browser and on wgpu-native. Tests: a stale buffer
  handle fails every call and logs its label; a buffer destroyed while a submission uses it is
  freed only after that submission completes; Validation reports the stale use.
- **H2 - textures, views and samplers**, as H1.
- **H3 - shaders, layouts, bind groups, pipelines, query sets and acceleration structures**, as H1;
  descriptors hold handles.
- **H4 - one retirement.** `GpuRetireQueue`, the per-pass retire paths and the `WaitIdle` fallbacks
  removed; `BindGroupCache` keyed on handles. Tests: replacing a resource mid-frame on every backend
  frees it after the frames using it, with no wait; the cache never returns a group for a destroyed
  input.
- **H5 - debugging.** `Device::Label`, the pretty printers and the natvis.
- **H6 - the other ids.** Physics joints and characters and navigation agents on Core handles;
  `RGHandle` and `VoiceHandle` moved onto them. Tests: an id kept past its removal does nothing
  when a new joint, character or agent took its slot.

Each phase runs the full lanes (clang, gcc, clang-shared, wasm and ASAN), and H1 and H4 are
measured in `build/clang-reldbg` with the render stress sample against the commit before them:
command recording within the noise.

## Gotchas

- **Recording threads look up handles while others create**: the pool's pages never move and each
  is published once, so a lookup is safe while the owner allocates. Destroying a resource a command
  is still being recorded with is the caller's mistake, as today; the device's retirement (Decision
  4) keeps the backend object alive past it in any case. Creating and destroying take the device's
  lock, as today.
- **A handle names a resource of one device**: two devices do not share tables, and the pool's id
  in the generation makes a handle from one fail its lookup on the other (Decision 1). The 8 bits
  allow 255 pools of a type alive at once (0 is never a pool's id).
- **The migration touches every renderer, so each phase keeps the tree green** by converting a
  type and all its callers together, never leaving a type half pointers and half handles.
- **Performance**: a lookup is an index and a generation compare per resource a command binds;
  measured in H1 and H4, and a backend may cache its object for a command it records more
  than once (a render bundle).

## Open questions

None. Decided 2026-11-22: `EntityHandle` stays as it is, a review noted in the 0.2 backlog; each
backend device keeps its own tables, with the pool's id in every handle's generation.

## Tests (the contract)

Each phase lands with its tests, listed per phase. Across the spec: a stale handle fails safely on
every backend and every call that takes one; a destroyed resource outlives the submissions that
used it and no longer; nothing waits for the GPU to replace a resource; and the samples, the sample
games and the editor render as before, which the user checks by eye.
