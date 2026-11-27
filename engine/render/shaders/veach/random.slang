// Random numbers for Veach's light transport: a PCG hash, deterministic per pixel, sample and dimension, so the
// same sample index gives the same numbers on every backend (ADR 0041).

uint pcg(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct Rng {
    uint state;
    [mutating] float next() {
        state = pcg(state);
        return float(state >> 8) * (1.0 / 16777216.0);
    }
};
