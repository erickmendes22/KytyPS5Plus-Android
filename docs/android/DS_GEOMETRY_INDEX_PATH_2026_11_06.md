# Demon’s Souls original index path (PPSA01341)

The earlier rollback alone does not fix missing geometry. The next delivered APK
must include a rendering change as well as the Wolverine memory change.

Executable examined: SHA256 d499226cdc4b074eb8ec72ac03ad469bb80f48e580164c08157e6fd0c608c2c5.
SELF executable physical base 0xFE50; read-only physical offset adjustment 0xFA80.

Static evidence from the actual executable:

- At image+0x976279, `doIndexBufferCulling` registers bool-table byte 0xCAA.
- At image+0x9762FA, `doSunShadowIndexBufferCulling` registers byte 0xCAB.
- At image+0xA0D6FB, the bool table is loaded from the render arguments. Byte
  0xCAA equal to 1 sets render-state +0x2DAEC, clears the original result through
  RBX and switches constants. The false branch skips those writes.
- At image+0xA086CE, render-state +0x2DAEC gates the job whose name is
  `XPR_IndexCullObjectsNormal`. Thus disabling the engine option avoids selecting
  and waiting on the GPU-derived index-culling result while retaining its original
  index path. This does not disable model creation or lower resolution.
- The exact 45-byte decision block, including its original branch displacement,
  runs in tests/manual/demons_geometry_branch_test.cpp. Both branches are checked:
  false retains the seeded result; true clears it and selects the GPU path.

The managed PackageCmdLineArgs.txt block uses +doIndexBufferCulling=0 and
+doSunShadowIndexBufferCulling=0 only for PPSA01341, after verifying both exact
null-terminated option names in a readable executable. A bounded stream scan
handles read-boundary splits. Configuration is atomic, idempotent and reversible,
preserves Nexus/demo route options and all unrelated bytes, and leaves the
executable unchanged. Modified managed blocks are rejected instead of overwritten.

The same named Bluepoint engine options are the normal play configuration of
AjncSystem/ShadowOfTheColossus-PC (commit7308102b0432746d710b6385ba0cb7386b9fb89b).
That is supporting engine precedent, not proof of the Demon’s Souls Android result.
The causal hypothesis is that the GPU-produced index path is responsible for the
missing geometry. The fallback's guest control flow is verified; the actual visual
result on Adreno still needs the user’s APK run. Do not claim it is already fixed.

The gpu-indirect=0 and targeted-readback=0 overrides that introduced invalid
command-buffer failures remain removed. Existing Bink, clock, shader-wait and
buffer rebind corrections remain active.
