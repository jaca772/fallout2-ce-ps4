#ifndef PS4_COMBAT_ASSIST_H
#define PS4_COMBAT_ASSIST_H

#ifdef __PS4__

#include "combat.h"
#include "object.h"

namespace fallout {

// PS4 gamepad combat assist functions (extracted from combat.cc).
int combatGamepadListTargets(Object** out, int maxCount, bool wholeMap);
int gamepadListInteractables(Object** out, int maxCount, bool wholeMap, int maxTiles, bool skipEmpty);
bool combatGamepadCanEndCombat();
void combatGamepadAimedAttack(Object* target);
void combatGamepadApproachAndAttack(Object* target, bool attackAfter);

} // namespace fallout

#endif // __PS4__

#endif // PS4_COMBAT_ASSIST_H
