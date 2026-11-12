// Netplay/mod address differences between USA FINAL.107 and Europe FINAL.113.
// Function entries and globals verified against the locally installed DOLs.
#pragma once
#include "ssne/ppc_context.h"
namespace ssne {
inline uint32_t game_address(uint32_t usa) {
    if(mem_read32(0x80000000u)!=0x47334250u) return usa; // G3BP
    switch(usa) {
    case 0x800F540Cu: return 0x800F5448u;
    case 0x800F5578u: return 0x800F55B4u;
    case 0x800F56A4u: return 0x800F56E0u;
    case 0x800FEDD4u: return 0x800FEE10u;
    case 0x8010C228u: return 0x8010C1E0u;
    case 0x8010CF00u: return 0x8010CEB8u;
    case 0x8010D70Cu: return 0x8010D6C4u;
    case 0x80117434u: return 0x801173ECu;
    case 0x8011855Cu: return 0x80118514u;
    case 0x80118560u: return 0x80118518u;
    case 0x8011E5F4u: return 0x8011E5ACu;
    case 0x801323D4u: return 0x8013238Cu;
    case 0x80117B9Cu: return 0x80117B54u;
    case 0x8014D63Cu: return 0x8014D5F4u;
    case 0x8014C090u: return 0x8014C048u;
    case 0x80150640u: return 0x801505F8u;
    case 0x8015070Cu: return 0x801506C4u;
    case 0x8015544Cu: return 0x80155404u;
    case 0x8015A40Cu: return 0x8015A3C4u;
    case 0x8015A504u: return 0x8015A4BCu;
    case 0x8015A54Cu: return 0x8015A504u;
    case 0x801664D8u: return 0x80166480u;
    case 0x80181590u: return 0x80181530u;
    case 0x801BA5ECu: return 0x801BA58Cu;
    case 0x801BA874u: return 0x801BA814u;
    case 0x801BF964u: return 0x801BF904u;
    case 0x801C0BACu: return 0x801C0B4Cu;
    case 0x801C193Cu: return 0x801C18DCu;
    case 0x80212268u: return 0x802121E8u;
    case 0x80213D48u: return 0x80213CC8u;
    case 0x80214110u: return 0x80214090u;
    case 0x802144C8u: return 0x80214448u;
    case 0x8021AB2Cu: return 0x8021AAACu;
    case 0x8028E824u: return 0x8028EF24u;
    case 0x8028E82Eu: return 0x8028EF2Eu;
    case 0x8028E830u: return 0x8028EF30u;
    case 0x8028E83Au: return 0x8028EF3Au;
    case 0x80291A54u: return 0x80292154u;
    case 0x80291BE0u: return 0x802922E0u;
    case 0x802AB3DCu: return 0x802ABADCu;
    case 0x802DA048u: return 0x802DA748u;
    case 0x802DA280u: return 0x802DA980u;
    case 0x802DA284u: return 0x802DA984u;
    case 0x802DA288u: return 0x802DA988u;
    case 0x802DA5B8u: return 0x802DACC0u;
    case 0x802DB0E0u: return 0x802DB7E8u;
    case 0x802DB0E4u: return 0x802DB7ECu;
    case 0x802DB1BCu: return 0x802DB8CCu;
    case 0x802E1BA0u: return 0x802E22A0u;
    default: return usa;
    }
}
}
