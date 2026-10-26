#pragma once

// Consume a title-specific startup deferral only after a real D3D11 device
// exists. No-op for hosts that retain ordinary background discovery.
void CompleteDeferredGpuDiscovery();
