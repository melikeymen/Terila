#pragma once

namespace terila::world {

    // Where players join the world and respawn after dying. A generated world names only a
    // column and its surface; a command replaces that with the exact point it was given,
    // facing and all.
    struct SpawnPoint {
        double x = 0.5;
        double y = 0.0;
        double z = 0.5;
        float yaw = 0.0f;
        float pitch = 0.0f;
        bool exact = false;
    };

} // namespace terila::world
