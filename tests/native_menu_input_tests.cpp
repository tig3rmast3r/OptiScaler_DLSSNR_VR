#include "../OptiScaler/menu/input/native_key_state.h"
#include <detours.h>
#include <cstdio>

namespace
{
int failures = 0, checks = 0;
bool physicalMouse = false, physicalRight = false;
SHORT WINAPI SuppressedGameState(int) { return 0; }
SHORT WINAPI SimulatedPhysicalState(int vk)
{
    return ((vk == VK_LBUTTON && physicalMouse) || (vk == VK_RIGHT && physicalRight))
        ? static_cast<SHORT>(0x8000) : 0;
}
void Check(bool value, const char* label)
{
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", label); }
}
}

int main()
{
    // Real exports and real detours, confined to this fixture process. No
    // SendInput, desktop event injection or modification of the game process.
    auto native = OptiInput::NativeAsyncKeyStateReader();
    auto game = &::GetAsyncKeyState;
    Check(native != nullptr, "Windows native physical-state export available");
    if (!native) return 1;
    Check(OptiInput::ReadPhysicalAsyncKeyState(-1) == 0, "negative key rejected");
    Check(OptiInput::ReadPhysicalAsyncKeyState(256) == 0, "out-of-range key rejected");
    Check(OptiInput::ReadPhysicalAsyncKeyState(VK_F24) == native(VK_F24), "actual native export semantics");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&game), SuppressedGameState);
    DetourAttach(reinterpret_cast<PVOID*>(&native), SimulatedPhysicalState);
    const auto attached = DetourTransactionCommit();
    Check(attached == NO_ERROR, "controlled native and game API detours attached");
    if (attached != NO_ERROR) return 1;

    physicalMouse = true;
    Check(::GetAsyncKeyState(VK_LBUTTON) == 0, "game-facing chain suppresses held mouse");
    Check((OptiInput::ReadPhysicalAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
          "owned menu still acquires held mouse");
    Check((OptiInput::ReadPhysicalAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
          "held mouse remains down across repeated polling");
    physicalMouse = false;
    Check(OptiInput::ReadPhysicalAsyncKeyState(VK_LBUTTON) == 0, "physical release acquired");
    physicalRight = true;
    Check(::GetAsyncKeyState(VK_RIGHT) == 0, "game-facing chain suppresses navigation key");
    Check((OptiInput::ReadPhysicalAsyncKeyState(VK_RIGHT) & 0x8000) != 0,
          "owned menu still acquires navigation key");
    physicalRight = false;
    Check(OptiInput::ReadPhysicalAsyncKeyState(VK_RIGHT) == 0, "navigation release acquired");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&native), SimulatedPhysicalState);
    DetourDetach(reinterpret_cast<PVOID*>(&game), SuppressedGameState);
    Check(DetourTransactionCommit() == NO_ERROR, "fixture detours fully removed");
    std::printf("Native menu input: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
