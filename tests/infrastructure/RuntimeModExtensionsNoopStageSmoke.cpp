// ARM-02: salvaged from RuntimeModExtensionsHistoryNoopSmoke.cpp.
//
// The original test had two halves. The second half compared
// advanceRuntimeModExtensions() output between the optimized and the reference
// history-query route; it cannot be built here. That function is now a concrete
// bool advanceRuntimeModExtensions(RuntimeGameSessionState&) in
// src/runtime/session/RuntimeSessionWorld.cpp, while the test drove it through a
// hand-written "small host". The host-generic stage it was written against used
// to live in simulation/mod_extensions/RuntimeModExtensionsRuntime.inl, which has
// been 0 bytes since the initial source import. Restoring that half means running
// it inside the interaction smoke (real session), tracked as ARM-03.
//
// What remains needs only the production header: the inert-history skip proof and
// its fixed-size complete-type cache. Those assertions are kept verbatim.
#include "simulation/mod_extensions/RuntimeModExtensions.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}

const std::string longPlain = std::string(512, 'x') + "-plain";
const std::string longShield = std::string(512, 'x') + "-shield";
const std::string unicodePlain = "\xE5\x8D\x95\xE4\xBD\x8D-plain";
const std::string unicodeShield = "\xE5\x8D\x95\xE4\xBD\x8D-shield";

std::string fixture(bool plainHasShield = false) {
    return "[VehicleTypes]\n0=PLAIN\n1=SHIELD\n2=ZEROSHIELD\n3=INNATE\n4=ENTRY\n5=MISSING\n6="
        + longPlain + "\n7=" + longShield + "\n8=" + unicodePlain + "\n9=" + unicodeShield
        + "\n[ShieldTypes]\n0=BASIC\n1=ZERO\n[Warheads]\n0=KEEP\n"
          "[BASIC]\nStrength=100\nInitialStrength=50\nPowered=yes\nSelfHealing=10%\nSelfHealing.Rate=0.1\n"
          "[ZERO]\nStrength=0\n[PLAIN]\nStrength=100\n"
        + (plainHasShield ? "Shield.Type=BASIC\n" : "")
        + "[SHIELD]\nStrength=100\nShield.Type=BASIC\n[ZEROSHIELD]\nStrength=100\nShield.Type=ZERO\n"
          "[INNATE]\nStrength=100\nAttachEffect.Duration=15\nAttachEffect.InitialDelay=0\n"
          "[ENTRY]\nStrength=100\nAttachEffect.Duration=15\nAttachEffect.DiscardOnEntry=yes\nAttachEffect.Delay=15\n"
          "[MISSING]\nStrength=100\nShield.Type=UNREGISTERED\n[KEEP]\nAttachEffect.Duration=30\n"
        + "[" + longPlain + "]\nStrength=100\n[" + longShield + "]\nStrength=100\nShield.Type=BASIC\n"
        + "[" + unicodePlain + "]\nStrength=100\n[" + unicodeShield + "]\nStrength=100\nShield.Type=BASIC\n";
}

void load(RulesDatabase& rules, bool plainHasShield = false) {
    IniFile ini;
    require(ini.loadFromString(fixture(plainHasShield)), "fixture INI parsing failed");
    require(rules.loadFromIni(ini), "fixture rule loading failed");
}

WorldObjectInstance tombstone(int id, std::string type = "PLAIN") {
    WorldObjectInstance object;
    object.instanceId = id;
    object.kind = WorldObjectKind::Unit;
    object.type = std::move(type);
    object.owner = "Alpha";
    object.x = 2; object.y = 3; object.cell = 1538;
    object.alive = false; object.health = 0; object.mission = "Destroyed";
    object.modExtensions.shieldOnline = false;
    return object;
}
}  // namespace

int main() {
    try {
        RulesDatabase rules;
        load(rules);
        const auto safe = tombstone(1);
        {
            RuntimeInactiveModExtensionNoopStage stage(rules, false);
            require(stage.canSkip(safe), "plain tombstone was not proved inert");
            require(stage.canSkip(safe) && stage.profileLookups() == 1, "same complete type repeated rule lookups");
            require(stage.canSkip(tombstone(2, "plain")), "case variant was not resolved by original rules");
            require(stage.cachedTypes() == 2, "raw complete type cache keys were folded together");
            require(stage.canSkip(tombstone(3, "")), "empty missing profile was not proved inert");
            require(stage.canSkip(tombstone(4, unicodePlain)), "non-ASCII plain type was not proved inert");
            require(!stage.canSkip(tombstone(5, unicodeShield)), "non-ASCII shield type incorrectly skipped");
            require(stage.canSkip(tombstone(6, longPlain)), "long plain type incorrectly rejected");
            require(!stage.canSkip(tombstone(7, longShield)), "long type suffix collision hid authored shield");
            require(!stage.canSkip(tombstone(8, "ZEROSHIELD")), "zero-strength registered shield initialization skipped");
            require(!stage.canSkip(tombstone(9, "INNATE")), "innate initialization skipped");
            require(stage.canSkip(tombstone(10, "MISSING")), "missing registered shield did not follow null lookup semantics");
            auto object = safe; object.alive = true; object.health = 0;
            require(!stage.canSkip(object), "alive health-zero object skipped");
            object = safe; object.modExtensions.initialized = true;
            require(!stage.canSkip(object), "initialized dead object skipped");
            object = safe; object.modExtensions.shieldOnline = true;
            require(!stage.canSkip(object), "default online-shield transition skipped");
            object = safe; object.modExtensions.effects.push_back({"KEEP", true, 30, 0});
            require(!stage.canSkip(object), "death/entry effect cleanup skipped");
            for (int index = 0; index < 600; ++index)
                require(stage.canSkip(tombstone(100 + index, "missing-type-" + std::to_string(index))), "cache overflow changed proof");
            require(stage.cachedTypes() == RuntimeInactiveModExtensionNoopStage::MaximumCachedTypes,
                "stage type cache exceeded its fixed entry limit");
        }
        {
            RuntimeHistoryQueryReferenceScope outer(true);
            RuntimeInactiveModExtensionNoopStage stage(rules);
            require(!stage.canSkip(safe) && stage.profileLookups() == 0, "shared reference did not bypass proof");
            {
                RuntimeHistoryQueryReferenceScope inner(false);
                RuntimeInactiveModExtensionNoopStage nested(rules);
                require(nested.canSkip(safe), "nested shared optimized scope failed");
            }
            require(runtimeHistoryQueryReference(), "shared reference scope was not restored");
        }
        std::cout << "Mod no-op stage proof passed: checks=" << checks
            << " completeTypeCacheLimit=" << RuntimeInactiveModExtensionNoopStage::MaximumCachedTypes
            << " caseVariant/unicode/longSuffix/zeroShield/innate/missing/onlineTransition/effectCleanup=yes"
            << " referenceScopeNesting=restored\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mod no-op stage proof failed: " << error.what() << '\n';
        return 1;
    }
}
