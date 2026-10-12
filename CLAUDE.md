# CLAUDE.md – securitybreach-minecraft-passthrough

## Project
- Fork van `chasmlol/SkyCraft` (Skyrim + Minecraft), omgebouwd voor FNAF: Security Breach (Unreal 4.27).
- Repo: `Mielesgames/securitybreach-minecraft-passthrough`. Lokale map heet nog `D:\Github\miside-minecraft-passthrough` (hernoemen lukt niet: `gradlew --stop` en java.exe sluiten).
- `sb/`: UE4SS C++ mod (`dllmain.cpp`, `Link.cpp/.h`; Bridge/Player/SharedMemory zijn nog leeg).
- `fabric/`: Minecraft 26.3 Fabric-mod (Java 25), o.a. `SkyClient.java`.
- `protocol/skycraft_protocol.h`: shared memory. Coördinaten zijn Minecraft-ruimte.
- `skse/`: de oorspronkelijke Skyrim-plugin, alleen als naslag (wordt niet gebouwd).
- UE4SS komt uit de submodule `RE-UE4SS` (main-branch, geen release): de `UE4SS.dll` in de game is die uit `build\Game__Shipping__Win64\bin`.

## Bouwen en installeren
- Game: `C:\Program Files (x86)\Steam\steamapps\common\Quarters\fnaf9\Binaries\Win64` (log: `UE4SS.log`, dumps: `crash_*.dmp`).
- Bouwen (cmake staat niet in PATH, die van Visual Studio 18 gebruiken):
  `cmake --build build --config Game__Shipping__Win64 --target SecurityBreachMinecraftPassthrough`
- Installeren: `build\sb\Game__Shipping__Win64\SecurityBreachMinecraftPassthrough.dll` kopiëren naar `Mods\SecurityBreachMinecraftPassthrough\dlls\main.dll` (moet `main.dll` heten).
- De DLL start Minecraft via `gradlew runClient` (pad overschrijfbaar met `SBMP_FABRIC_DIR`) en zet `SKYCRAFT_PASSTHROUGH=1`.
- Claude kan de game niet zelf draaien: elke wijziging aan `sb/` is pas getest als jij hem in de game hebt geprobeerd.

## Hoe de mod werkt
- Alles wat Unreal-objecten aanraakt draait op de game-thread via `RegisterEngineTickPreCallback` (`on_unreal_init`). **Niet in `on_update`**: dat is UE4SS' eigen thread.
- Controller `MainGamePC_C` wordt gezocht met `FindAllOf` en daarna via een `FWeakObjectPtr` hergebruikt. Offsets: `+0x6F0` FirstPersonCharacter (Gregory), `+0x290` ControlRotation.
- F6 = Minecraft-modus aan/uit, F7 = noodstop, watchdog (3 s zonder Minecraft-frames → uit).
- Bij F6 vraagt de mod Minecraft om naar Gregory te teleporteren (`teleportSeq`). Gregory wordt pas weer verplaatst als Minecraft dat bevestigd heeft (`teleportAck`) én binnen 35 eenheden van hem staat (`driving`). Een sprong van meer dan 210 eenheden in één frame → opnieuw synchroniseren in plaats van volgen.
- Minecraft bepaalt alleen X/Z; de hoogte blijft die van Unreal. Game-WASD staat uit via `Controller:SetIgnoreMoveInput` (dat is een teller: één keer per wissel aanroepen).
- Mapping: `kUnitsPerBlock` = 70, `kZSign = +1.0`, yaw = `kZSign * yaw - 90`.
- Minecraft-kant: passthrough-modus in `SkyClient` (geen `holdUntilReady`, Y vastgezet op Gregory's hoogte).

## De crash van 8 oktober (opgelost in code, nog niet in de game getest)
- Alle 11 dumps zijn dezelfde crash: schrijven naar `0x98` in memcpy, direct na F6, tijdens de eerste `K2_SetActorLocation`.
- Wat er gebeurde: de verplaatsing liet een Blueprint-event afgaan op een object in het level (overlap-achtig event met een hit-result), dat een interface-event aanriep op een ander object zonder "persistent ubergraph frame" → schrijven naar null+0x98. Welke Blueprints het zijn is niet te zien: de namen staan niet in de dump.
- Twee dingen die de mod fout deed en die nu anders zijn:
  1. De aanroep kwam van UE4SS' eigen thread (`on_update`), niet van de game-thread.
  2. Na F6 volgde Gregory meteen Minecrafts oude positie, waardoor hij elke keer ~2,6 m terugsprong naar het punt waar de save laadde.
- Niet bewezen welke van de twee de crash gaf. Als hij terugkomt: `tools/crash_stack.py` op de nieuwe dump.
- De hit-buffer deed er niet toe: UE4SS' `K2_SetActorLocation` gebruikt `SweepHitResult` niet (`FHitResult` is daar een lege struct).

## Stand na de test van 12 oktober
- Lopen werkt en de crash is weg (game-thread + wachten op `teleportAck`).
- "Bevriezen" was de E-toets: die opende Minecrafts inventory, en met een scherm open staat de Minecraft-speler stil. E wordt niet meer doorgestuurd, en een open Minecraft-scherm (`kMcScreenOpen`) wordt met Escape gesloten.
- F6 wordt genegeerd zolang Minecraft nog niet in zijn wereld is (opstarten duurt ~40 s).
- Muren: `kSweepWalls` staat aan. Per frame: vlakke sweep → bij blokkade 45 omhoog, vooruit, weer omlaag (trap/helling) → anders per as langs de muur schuiven → bij meer dan 20 eenheden verschil Minecraft terugzetten op Gregory. **Nog niet in de game getest.** Verwacht: schuiven langs muren is schokkerig, omdat elke sync Minecrafts speler even stilzet.

- Muren en ramps zijn door jou getest en werken. Stilvallen bij een muur komt van "Gregory was blocked"-syncs; of Minecraft daarnaast zelf hapert, logt de mod nu als `SBMP Minecraft hitch`.
- Intro na New Game (Gregory in Freddy): `controller+0x6F0` is dan null. De mod stuurde toen geen heartbeat, dus Minecraft bleef op zijn beginscherm. Nu gaat de heartbeat elke tick, ook zonder Gregory; lopen kan pas als Gregory er is. **Nog niet getest.**

- New Game, tweede test: met alleen een heartbeat viel Steve de void in en bleef doodgaan, want Minecrafts passthrough-modus deed niets zolang de host niet "in game" was. `SkyClient.passthroughPhysics` parkeert de speler nu zolang er geen host-speler is, en laat hem meteen respawnen als hij toch doodgaat.
- Na New Game blijft `controller+0x6F0` leeg, ook na de intro. De mod valt dan terug op de `Pawn` van de controller (via reflectie). Getest: tijdens de intro is de pawn `Freddy_C`, daarna `Gregory_C`, en lopen werkt. Steve valt niet meer.
- Java-wijzigingen controleren met `fabric\gradlew compileClientJava` (kan alleen als Minecraft niet draait).

## Minecraft in beeld (stap 1: overlay, nog niet getest)
- `sb/src/Overlay.cpp` is een port van `skse/src/Overlay.cpp`: hookt `IDXGISwapChain::Present` (vtable via een wegwerp-swapchain) en tekent Minecrafts beeld (hand, HUD, schermen, doorzichtige achtergrond) over het frame. Alleen D3D11, alleen op het `UnrealWindow`-venster, alleen in Minecraft-modus.
- Minecraft levert die beelden al via `FrameExporter` (overlay-triplebuffer in shared memory); de mod geeft de echte back-buffer-grootte door als viewport.
- De game draait standaard op **DirectX 12**; dan wordt er niets getekend. Met de Steam-startoptie `-dx11` werkt de overlay (getest 12 okt: hand en HUD zichtbaar). Al SkyCrafts rendercode is D3D11, dus `-dx11` blijft nodig.
- Nog niet: blokken en de wereld van Minecraft in de 3D-scène, en Steve's lichaam (F5). Voorbeeld: `skse/src/WorldRender.cpp` (3400 regels).

## Invoer (`sb/src/Input.cpp`, nog niet getest)
- De mod zit vóór de message-handler van het gamevenster (`SetWindowLongPtr(GWLP_WNDPROC)`), dus op de game-thread. In Minecraft-modus gaan toetsen, muisknoppen en het scrollwiel naar Minecraft en ziet de game ze niet.
- Uitzonderingen: Space gaat naar allebei (springen is nog van Unreal), Escape buiten een Minecraft-scherm is het pauzemenu van de game, F5/F6/F7/F11 gaan niet naar Minecraft.
- Minecraft-scherm open (`kMcScreenOpen`): ruwe muisbeweging (`WM_INPUT`) wordt Minecrafts cursor (`kInCursor`, getekend door de overlay), de camera van de game draait dan niet, en getypte tekens gaan mee (`kInText`).
- Crouch: Gregory's crouch volgt Minecrafts sneak (Shift) via `Character:Crouch`/`UnCrouch`.
- Test 12 okt: E, scrollen en slaan werken. Niet goed: rondkijken werkte niet meer en de cursor in de inventory bewoog niet. Vermoeden (niet bewezen): de game had de muis niet "gevangen" en de mod slikte de klik in waarmee de game hem weer vangt. Nu gaat een klik ook naar de game zolang `GetCapture()` niet het gamevenster is, en zonder ruwe muisbeweging gebruikt de cursor `WM_MOUSEMOVE`. Elke 5 s logt de mod `SBMP input: raw moves …, plain moves …, game has the mouse: …`.

- Test daarna: rondkijken, inventory-cursor en vliegen werken. Het log toont dat de game de muis als ruwe beweging (`WM_INPUT`) leest en de muis vast had.
- Echte muisaanwijzer liep naar het tweede scherm terwijl een Minecraft-scherm open was: de game zag de muis dan niet en zette de verborgen aanwijzer niet meer in het midden. De mod doet dat nu zelf (`SetCursorPos`) bij elke ruwe beweging. Nog niet getest.

## Minecrafts wereld in beeld (`sb/src/World.cpp`)
- Getest 12 okt: geplaatste blokken staan goed in de wereld (plek, vorm, texturen).
- De Present-thread leest de render-ring uit (`world::consume`, elke frame, ook als er niets getekend wordt: een volle ring laat Minecraft ~1,4 s per frame wachten). Verwerkt: `kRenAtlas`, `kRenAtlasRegion`, `kRenSection`, `kRenClearAll`, `kRenTexture`, `kRenScene` (entities en particles, elke frame opnieuw). Nog niet: `kRenAvatar` (Steve's lichaam, F5), lichten.
- Tekenen gebeurt over het afgewerkte frame, vóór de HUD-overlay, met een eigen dieptebuffer (reversed-Z zoals Unreal).
- **Diepte van de game (nog niet getest):** de mod hookt `ID3D11DeviceContext::ClearDepthStencilView` (vtable-index 53) en onthoudt per frame de grootste dieptebuffer met de vorm van de back buffer die de game wist (bij gelijke grootte de eerste). De pixelshader leest die en gooit pixels weg die achter de geometrie van de game liggen. Aannames: near plane 10 (`kNearPlane`), en de dieptebuffer beslaat het hele beeld. Het log zegt `SBMP world: using the game's depth buffer, …`.
- Camera: de game-thread vraagt elke tick `GetCameraLocation`/`GetCameraRotation`/`GetFOVAngle` aan de `PlayerCameraManager` (`world::set_camera`). Dat loopt een frame achter op wat getekend is.
- Assen: Unreal X = Minecraft x·70, Unreal Y = Minecraft z·70, Unreal Z = Minecraft y·70.
- Hoogte: Minecrafts positie is de voeten, die van Unreal het midden van de capsule. De mod trekt daarom `CapsuleHalfHeight` af (en telt hem er bij vliegen weer bij op).
- De event-ring (klappen, dood, explosies) wordt nog weggegooid (`discard_unused_rings`).
- Pauzemenu van de game (`GameplayStatics:IsGamePaused`): dan tekent de mod niets en gaat alle invoer weer naar de game. Nog niet getest.

## Vliegen (getest: werkt)
- Minecraft vliegt (`kMcFlying`: creative of spectator) → Minecraft bepaalt ook de hoogte. `SkyClient.passthroughPhysics` zet Y dan niet meer vast, en de mod zet Gregory's `CharacterMovement` op `MOVE_Flying` en beweegt hem in 3D (sweep, per as schuiven).
- Spectator (`kMcSpectator`, nieuwe vlag in het protocol) gaat zonder sweep, dus door muren.
- Stopt Minecraft met vliegen, dan gaat Gregory naar `MOVE_Falling` en volgt Minecraft zijn hoogte weer.

## Deuren, slaan en ooghoogte (in `dllmain.cpp`, nog niet getest)
- Rechtermuisknop buiten een Minecraft-scherm wordt door `Input.cpp` vastgehouden tot de game-tick beslist: richt Minecraft op een eigen blok (`WorldEntities.hasSelection`), dan gaat de klik naar Minecraft. Anders traceert de mod 315 eenheden vanuit de camera. Heeft de geraakte actor `ForceDoorOpen`/`ForceDoorClose` (`BP_DoorMaster_C`), dan wisselt de deur (ook als hij op slot zit). Heeft hij `OnPlayerInteract` (de `InteractibleInterface` van het spel), dan wordt dat aangeroepen. Anders gaat de klik alsnog naar Minecraft.
- Linkermuisknop gaat altijd naar Minecraft; daarnaast zoekt de mod met een dikke trace (210 eenheden) een `Character` voor je. Die krijgt `Stun()` als hij dat heeft (`StunableInterface`) en `LaunchCharacter` van je af.
- Test 12 okt: Freddy wegslaan werkt, gewone deuren werken. Deuren met een sleutel gingen meteen weer dicht (automatische deuren: `UDoorComponent` sluit ze voor wie niet mag). De mod haalt nu eerst het slot weg (`SetLockedForPlayer(false)`, `SetSecurityLevel(0)`, `SetDoorRequiredItem(None)`, `ClearPlayerBlocker`) en zet de automaat voor de speler uit (`SetAutomaticDoorEnableForPlayer(false)`), zodat de deur blijft zoals Steve hem achterliet. Nog niet getest.
- De schoonmaakbot was niet te raken. `CautionBot_C` is een gewone `APawn` zonder `LaunchCharacter`; zulke pawns schuift de mod nu zelf 0,3 s weg (`update_shove`, met sweep). Of dat de bot is die jij bedoelde weet ik niet: het log zegt nu ook wat een slag wél raakte (`SBMP hit: the swing only reached …`).
- Bij gebruik van iets in de game stuurt de mod `kInSwing` (nieuw invoertype) zodat Steve's arm zwaait.
- Het log noemt wat er geraakt of gebruikt is (`SBMP use: …`, `SBMP hit: …`): daaraan is te zien welke klassen het spel voor hekjes en personages gebruikt.
- Ooghoogte: Gregory kan niet bukken terwijl Unreals beweging uit staat, en hij is kleiner dan Steve. De mod verschuift hem daarom verticaal zodat de camera op Minecrafts ooghoogte (`McState.eyeY`) staat; dat zakt mee bij sneaken en is de hoogte waarvandaan Minecraft mikt.

## Personages en Minecraft-blokken (`sb/src/Actors.cpp`, nog niet getest)
- Elke seconde zoekt de mod alle `Pawn`s (`FindAllOf`), en schrijft die binnen 48 blokken elke tick in de `ActorTable`. Minecraft (SkyCrafts `SkyCombat`/`ProxySync`) houdt op elke positie een onzichtbare stand-in-entity; die drukt drukplaten in.
- Minecraft stuurt per sectie welke blokken vast zijn (`kRenSolids`); `World.cpp` bewaart die (`world::solid_at`). Staat een personage in zo'n blok, dan duwt de mod het er horizontaal weer uit (`push_out_of_blocks`), zodat een ijzeren deur of muur het tegenhoudt. De AI weet niets van de blokken en blijft er dus tegenaan lopen.
- Klappen op een stand-in komen als `kEvHitActor` terug, maar de event-ring wordt nog weggegooid; slaan loopt via de eigen trace van de mod.

## Toegevoegd op verzoek, allemaal nog niet getest
- **F5:** F5 gaat naar Minecraft. `World.cpp` tekent Steve's lichaam (`kRenAvatar`, relatief aan Minecrafts voeten, `world::set_player_position`); dat werkt (getest). De eerste poging voor de camera (Gregory's `FirstPersonCamera` verplaatsen) deed niets: je keek in Steve's hoofd, waarschijnlijk omdat Gregory zijn camera elke frame terugzet. Nu zet de mod de `PlayerCameraManager` op `CameraStyle = FreeCam` met `FreeCamDistance` = Minecrafts camera-afstand en `FreeCamOffset.z` op Minecrafts ooghoogte; bij terugkeer `Default`. Nog niet getest. Gregory zelf is verborgen (`SetActorHiddenInGame`) zolang Minecraft rijdt.
- **Events uit Minecraft** (`SecurityBreachLink::read_event`, `handle_events`): `kEvHitActor` (klap op een stand-in: wapens, pijlen, golems, vuur) en `kEvExplosion` (TNT, creepers) gooien de betreffende personages weg (`hit_character`). Een klap die de eigen slag-trace van de mod al gaf wordt 400 ms genegeerd.
- **IJzeren golems** vallen de stand-ins aan (`IronGolemMixin`, in `skycraft.mixins.json`).
- **Night vision:** nieuwe vlag `kMcNightVision`; de mod maakt het afgewerkte beeld van de game lichter (×4 plus een bodem) en belicht Minecrafts eigen dingen volledig. Het is geen echt licht: wat in het beeld helemaal zwart is blijft vlak.
- **Doel van het project (jouw woorden): het hele spel moet als Steve te spelen zijn.** Rechtermuisknop moet dus alles doen wat de interactietoets van het spel doet, en meteen.
- **Vasthoud-interacties** (cadeaus, sommige schakelaars: `GetInteractibleType` geeft `Hold`) worden direct afgemaakt: na `OnPlayerInteract` roept de mod de `…PlayerInteractHoldComponent…OnHoldCompleted…`-handler van het object aan, of anders `OnCollect` (`complete_hold`). Er is ook een derde, bredere trace voor kleine dingen.
- **Commando's in de Minecraft-chat** (`SkyCraftClient`, event `kEvCommand`, `host_command` in `dllmain.cpp`): `/freddy`, `/chica`, `/monty`, `/roxy`, `/vanny`, `/vanessa`, `/moon`, `/sun`, `/bot`, `/endo`, `/musicman` of `/tp <naam>` halen het bestaande personage naar de speler; `/summon <naam>` maakt een nieuwe (`GameplayStatics:BeginDeferredActorSpawnFromClass` + `FinishSpawningActor` + `SpawnDefaultController`). De naam wordt gezocht in de klassenamen van de pawns. Nog niet getest; of een gespawnde animatronic iets doet is onbekend.
- **TNT-kettingreactie:** SkyCrafts `SkyDigBlast` zag de vloer van het spel als onverwoestbaar gebouw en liet die elke explosie opslokken. Voor Security Breach staat dat uit (`SkyDig.PASSTHROUGH`), dus explosies zijn weer gewoon Minecraft.
- **Omlijning op lucht:** SkyCraft omlijnt ook de plek waar een blok zou komen als je op de wereld van het andere spel richt. Voor Security Breach uitgezet (`WorldExporter.selection`): alleen echte blokken.
- Getest door jou: Freddy botst tegen pistons en andere blokken; de omlijning werkt op blokken.
- **Nacht:** zombies verbrandden. In passthrough zet `SkyClient` de Minecraft-wereld elke 200 ticks op middernacht (`time set midnight` via de geïntegreerde server). `SkyState.gameHour` wordt door de Minecraft-kant niet voor de tijd gebruikt.
- **Mobs konden niet lopen:** de collision wordt wel geïnjecteerd (ze staan op de vloer), maar de padzoeker ziet alleen lucht. `WalkNodeEvaluatorMixin` laat een cel als vast tellen als de geometrie van het spel de bovenste helft vult of als hij een vloer draagt die in de onderste helft van de cel erboven ligt. Alleen in passthrough, alleen binnen de gescande regio's (3×3×3 rond de speler). Nog niet getest.
- **Test 12 okt, 21:50:** Minecraft start, golems lopen (draaien soms rondjes) en slaan animatronics, night vision werkt, `/freddy` werkt, rechtermuisknop werkt op save-station, oplaadstation, deuren en verzamelobjecten.
- **F5-camera:** `CameraStyle` is geen gereflecteerde eigenschap, dus de vorige poging deed niets. Nu via `PlayerController:ClientSetCameraMode("FreeCam"/"Default")`, met `FreeCamDistance`/`FreeCamOffset` (die bestaan wel). Nog niet getest.
- **Dubbele interacties:** bij vasthoud-objecten startte de mod het vasthouden én maakte het af, waarna de timer van het spel het nog eens afmaakte. Nu wordt alleen het afmaken aangeroepen. Nog niet getest; of dit alle dubbele gevallen dekt is onbekend.
- **`/summon <naam>` en `/tp <naam>`** zijn nu echte client-commando's (met autofill) in plaats van onderschepte chatregels; de onderschepping kwam bij `/summon` niet aan. Nog niet getest.
- **Losse voorwerpen wegslaan:** raakt de slag een component dat physics simuleert (`IsSimulatingPhysics`), dan krijgt het `AddImpulse` als snelheidsverandering (`hit_loose_object`). Nog niet getest.
- **Test 12 okt, later:** F5 van achteren werkt, dubbele interacties zijn weg, `/freddy` werkt.
- **F5 vooraanzicht** (cameraMode 2): de mod stuurt Minecraft dan de omgekeerde kijkrichting (yaw + 180, pitch omgekeerd), zodat de FreeCam-camera vóór Steve komt en terugkijkt. Nog niet getest; slaan en gebruiken mikken in die stand nog langs de camera in plaats van langs Steve's blik.
- **Commando's met autofill:** verplaatst naar de server (`SkyCraft.onInitialize`, `CommandRegistrationCallback`): `/<naam>`, `/teleport <naam>` (dus ook `/tp`, dat alleen doorverwijst) en `/summon <naam>` als extra takken van Minecrafts eigen commando's. Nog niet getest.
- **Dingen van het spel slopen door te slaan:** cutouts (herkend aan `cutout`/`standee`/`cardboard` in de naam van de actor of zijn mesh; er is geen eigen klasse voor gevonden, dus dit is een gok) en deuren. Creative: één slag, behalve met een zwaard (dan niets, vlag `kMcHoldsSword`). Survival: 5 slagen voor een cutout, 12 voor een deur; de telling begint opnieuw bij een ander doel of na 3 s. Bij het breken `K2_DestroyActor`, plus Minecrafts breek-particles en -geluid (`kInBreakEffect`). Nog niet getest.
- **Gebruiken:** naast de pawn-trace een tweede trace op het zichtbaarheidskanaal (`collision::trace_visible`), voor dingen waar je doorheen kunt lopen (knoppen).
- **Pijlen, gedropte items, drietanden, breekscheuren en de omlijning van het blok waar je op richt:** `World.cpp` leest elke frame `WorldEntities` (seqlock) en bouwt er driehoeken en lijnen van (`build_dynamic`, overgenomen uit `skse/src/WorldRender.cpp`).
- **Krappe deuren:** de muren komen afgerond op kwart blokken binnen, dus elke deuropening is tot een kwart blok per kant smaller. Steve botst daarom met een smallere romp (straal 0,18 in plaats van 0,3, `SkyCollider`).

## Nog te doen
1. Alles hierboven testen, plus deuren met sleutel, bots slaan, de armzwaai, de drukplaat-val en personages die tegen blokken botsen.
2. Fakkels en ander Minecraft-licht het spel laten verlichten (`kRenLights` wordt nog overgeslagen).
4. Hitboxes fijner dan een kwart blok, zodat de smallere romp niet meer nodig is.

## Ontwerpregel: wie bepaalt de physics
- Minecraft-modus aan (F6) = Minecraft-physics hebben voorrang, inclusief vliegen in creative en spectator-modus.
- Minecraft-modus uit = gewoon Security Breach, zonder iets van Minecraft in beeld of in de besturing.

## Hitboxes (`sb/src/Collision.cpp`, getest 12 okt: lopen, springen en blokken op de vloer zetten werken)
- In Minecraft-modus tast de mod de omgeving af en stuurt die als SkyCrafts collision-regio's (`kColTris` voor de speler, `kColRegion` voor mobs) door de collision-ring. Minecraft draait dan zijn gewone physics (lopen, springen, vallen, vliegen) en de mod zet Gregory elke frame op Minecrafts positie, zonder sweep, met zijn `CharacterMovement` op `MOVE_Flying` zodat Unreal hem met rust laat.
- Aftasten: `KismetSystemLibrary:BoxTraceSingleByProfile` met profiel `Pawn` (via reflectie, geen vaste parameter-layout). Een regio is 8³ blokken; een geblokkeerde kubus wordt in achten gesplitst tot cellen van een kwart blok. Van de bovenste cel van elke stapel bepaalt een `LineTraceSingleByProfile` de exacte vloerhoogte.
- Regio's: 3×3×3 rond de speler, dichtstbijzijnde eerst, 4 ms per tick zolang er onbekende zijn, daarna 0,6 ms; elke regio wordt na 5 s opnieuw bekeken (deuren) en alleen opnieuw verstuurd als hij veranderd is.
- Wie rijdt staat in `SkyState.flags`: `kSkyHostDrives` (nieuw) = de game beweegt Gregory zelf en Minecrafts speler wordt op hem vastgepind (`SkyClient.passthroughPhysics`). Zonder die vlag draait Minecraft zijn normale SkyCraft-pad, inclusief `holdUntilReady` (wacht na elke teleport tot de regio's onder de speler bekend zijn).
- Vangnet: is Minecrafts speler 3 s in de lucht zonder te vliegen, dan gaat alles terug naar de laatste plek waar hij stond.
- Space gaat niet meer naar de game: springen is van Minecraft.
- Andere pawns (Freddy, animatronics, bots) worden bij het aftasten overgeslagen: raakt een box-trace een actor met een `GetController`-functie, dan wordt opnieuw getraced met die actor in `ActorsToIgnore`. Nog niet getest.
- Bekende zwakke plekken: Steve (1,8 blok hoog, 0,6 breed) is groter dan Gregory, dus lage doorgangen kunnen te krap zijn; hellingen zijn treetjes van een kwart blok breed.
- De oude aanpak (Gregory met een sweep achter Minecraft aan, opstapje, langs muren schuiven, terugzetten bij blokkade) is hiermee vervangen en verwijderd.

## Wensen
- Deuren van Security Breach openen als Steve: rechtermuisknop op een deur → deur gaat open.
- Animatronics van je af kunnen slaan als Steve.
- Freddy berijden als een Minecraft-paard met zadel: rechtermuisknop op Freddy, daarna bestuur je hem.
