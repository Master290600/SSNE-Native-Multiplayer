// PAL original native weapon audio call sites, verified against recompiled
// functions and all 12 weapon vtables in the captured 64-MiB level fixture.
// caller is the original return address at the audio API entry. instance_register
// retains function-entry r3 for every path up to that call (verified assignments).
// These are audio cues, not proof of a shot. Some use persistent handles even
// though they enter the one-shot API; preserve native explicit stops/retirement.
// Evidence: build-coop-tests/weapon-effects-v36/weapon-audio-callers.json.
// Include inside the native effect namespace after <array>/<cstdint>.
struct WeaponAudioCall { uint32_t caller,sound,flags; unsigned weapon,instance_register; };
inline constexpr std::array<WeaponAudioCall,47> weapon_audio_calls{{
    {0x80053144u,422u,0u,10u,31u}, // original 80053064
    {0x80053294u,423u,0u,10u,31u}, // original 8005320C
    {0x800535F8u,430u,0u,0u,31u}, // original 800535BC
    {0x8005368Cu,429u,0u,0u,31u}, // original 80053634
    {0x80053700u,432u,0u,0u,31u}, // original 80053634
    {0x800537E4u,432u,0u,0u,31u}, // original 80053634
    {0x80054054u,431u,0u,0u,31u}, // original 80053634
    {0x80054080u,428u,0u,0u,31u}, // original 80053634
    {0x800546A8u,426u,0u,7u,31u}, // original 8005459C
    {0x800547A8u,411u,0u,7u,31u}, // original 8005459C
    {0x800548B8u,426u,0u,7u,31u}, // original 800547BC
    {0x80054BE4u,426u,0u,7u,31u}, // original 800549C8
    {0x8005504Cu,412u,1u,7u,31u}, // original 80054C00
    {0x800550CCu,411u,0u,7u,31u}, // original 80054C00
    {0x800552B4u,411u,0u,7u,31u}, // original 8005527C
    {0x800557D8u,405u,0u,5u,31u}, // original 800556E4
    {0x80055A14u,405u,0u,5u,31u}, // original 80055920
    {0x80055C54u,405u,0u,5u,31u}, // original 80055B5C
    {0x80056104u,416u,1u,1u,31u}, // original 80056098
    {0x80056140u,416u,1u,1u,31u}, // original 80056098
    {0x8005643Cu,417u,1u,1u,31u}, // original 800561AC
    {0x80057784u,409u,1u,4u,31u}, // original 800576B4
    {0x800577F0u,407u,1u,4u,31u}, // original 800576B4
    {0x800578A0u,408u,1u,4u,31u}, // original 800576B4
    {0x80057960u,407u,1u,4u,31u}, // original 800576B4
    {0x80057D2Cu,409u,1u,4u,31u}, // original 80057C5C
    {0x80057D98u,407u,1u,4u,31u}, // original 80057C5C
    {0x80057E48u,408u,1u,4u,31u}, // original 80057C5C
    {0x80057F08u,407u,1u,4u,31u}, // original 80057C5C
    {0x80059B70u,420u,1u,9u,31u}, // original 80059B18
    {0x80059FC4u,421u,1u,9u,31u}, // original 80059F8C
    {0x8005A310u,401u,0u,9u,31u}, // original 8005A158
    {0x8005A724u,402u,0u,9u,31u}, // original 8005A6C0
    {0x8005CE90u,415u,1u,6u,31u}, // original 8005CDEC
    {0x8005D0ECu,415u,1u,6u,31u}, // original 8005D048
    {0x8005D33Cu,415u,1u,6u,31u}, // original 8005D298
    {0x8005DD18u,433u,0u,11u,30u}, // original 8005DC60
    {0x8005DF58u,434u,0u,11u,31u}, // original 8005DD3C
    {0x8005E1C4u,335u,0u,11u,31u}, // original 8005E190
    {0x8005E1E8u,335u,0u,11u,31u}, // original 8005E190
    {0x8005EB38u,413u,0u,3u,28u}, // original 8005E9D0
    {0x8005EC44u,414u,1u,3u,28u}, // original 8005E9D0
    {0x80060178u,435u,0u,8u,31u}, // original 8005FF38
    {0x80060380u,418u,1u,8u,31u}, // original 8005FF38
    {0x800615E4u,425u,0u,2u,31u}, // original 800612A0
    {0x80061848u,425u,0u,2u,31u}, // original 800615FC
    {0x80061CE4u,425u,0u,2u,31u}, // original 80061C88
}};

// Existing true loop API 8010EB9C: separate start/stop event stream.
inline constexpr std::array<WeaponAudioCall,4> weapon_audio_loop_calls{{
    {0x800578D0u,406u,1u,4u,31u}, // original 800576B4
    {0x80057E78u,406u,1u,4u,31u}, // original 80057C5C
    {0x80061378u,419u,0u,2u,31u}, // original 800612A0
    {0x800616D8u,419u,0u,2u,31u}, // original 800615FC
}};

// Explicit stops8010EDFC. handle_offset is relative to that exact instance.
struct WeaponAudioStopCall { uint32_t caller; unsigned weapon,instance_register; uint32_t handle_offset; };
inline constexpr std::array<WeaponAudioStopCall,43> weapon_audio_stop_calls{{
    {0x800531C4u,10u,31u,0x22Cu}, // original 80053064
    {0x80053618u,0u,31u,0x230u}, // original 800535BC
    {0x80053680u,0u,31u,0x230u}, // original 80053634
    {0x800536F4u,0u,31u,0x230u}, // original 80053634
    {0x80054048u,0u,31u,0x230u}, // original 80053634
    {0x80054784u,7u,31u,0x250u}, // original 8005459C
    {0x80054798u,7u,31u,0x24Cu}, // original 8005459C
    {0x8005490Cu,7u,31u,0x250u}, // original 800547BC
    {0x80054AF0u,7u,31u,0x250u}, // original 800549C8
    {0x800550B8u,7u,31u,0x24Cu}, // original 80054C00
    {0x800552A0u,7u,31u,0x24Cu}, // original 8005527C
    {0x800562D4u,1u,31u,0x248u}, // original 800561AC
    {0x80057114u,4u,31u,0x274u}, // original 800570EC
    {0x80057128u,4u,31u,0x270u}, // original 800570EC
    {0x80057764u,4u,31u,0x27Cu}, // original 800576B4
    {0x800577D4u,4u,31u,0x278u}, // original 800576B4
    {0x80057804u,4u,31u,0x270u}, // original 800576B4
    {0x80057818u,4u,31u,0x274u}, // original 800576B4
    {0x80057944u,4u,31u,0x278u}, // original 800576B4
    {0x80057974u,4u,31u,0x270u}, // original 800576B4
    {0x80057988u,4u,31u,0x274u}, // original 800576B4
    {0x80057D0Cu,4u,31u,0x27Cu}, // original 80057C5C
    {0x80057D7Cu,4u,31u,0x278u}, // original 80057C5C
    {0x80057DACu,4u,31u,0x270u}, // original 80057C5C
    {0x80057DC0u,4u,31u,0x274u}, // original 80057C5C
    {0x80057EECu,4u,31u,0x278u}, // original 80057C5C
    {0x80057F1Cu,4u,31u,0x270u}, // original 80057C5C
    {0x80057F30u,4u,31u,0x274u}, // original 80057C5C
    {0x80058484u,4u,31u,0x270u}, // original 8005845C
    {0x80058498u,4u,31u,0x278u}, // original 8005845C
    {0x800584ACu,4u,31u,0x27Cu}, // original 8005845C
    {0x800584C0u,4u,31u,0x274u}, // original 8005845C
    {0x80059FB0u,9u,31u,0x228u}, // original 80059F8C
    {0x8005A710u,9u,31u,0x228u}, // original 8005A6C0
    {0x8005AD24u,9u,31u,0x228u}, // original 8005ACFC
    {0x8005DBE8u,11u,31u,0x234u}, // original 8005DB6C
    {0x8005E1B0u,11u,31u,0x234u}, // original 8005E190
    {0x8005FED0u,8u,29u,0x288u}, // original 8005FE04
    {0x80060250u,8u,31u,0x288u}, // original 8005FF38
    {0x8006116Cu,2u,31u,0x244u}, // original 80061148
    {0x800615D0u,2u,31u,0x244u}, // original 800612A0
    {0x80061834u,2u,31u,0x244u}, // original 800615FC
    {0x80061CD0u,2u,31u,0x244u}, // original 80061C88
}};
