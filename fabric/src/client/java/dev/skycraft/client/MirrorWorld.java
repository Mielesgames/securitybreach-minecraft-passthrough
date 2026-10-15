package dev.skycraft.client;

import dev.skycraft.SkyCraft;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.Difficulty;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.LevelSettings;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.presets.WorldPreset;

/** Opens (or creates) the void "mirror" world automatically once Skyrim is connected. */
public final class MirrorWorld {
	private static final ResourceKey<WorldPreset> PRESET =
		ResourceKey.create(Registries.WORLD_PRESET, Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "mirror"));
	private static boolean attempted;
	private static long lastLog;
	// /join: a friend's world for this session (the e4mc link their "Open to LAN" shows); null: our own.
	private static @org.jspecify.annotations.Nullable String sessionJoin;
	// Shown in chat once the player is in a world again (why they're back in their own, ...).
	private static @org.jspecify.annotations.Nullable String pendingNote;

	private MirrorWorld() {
	}

	/**
	 * The address in config/skycraft.properties ({@code join=abc-def.e4mc.link}), if any. Written
	 * with the template below the first time, so there's something to fill in.
	 */
	private static @org.jspecify.annotations.Nullable String joinAddress(Minecraft minecraft) {
		java.nio.file.Path file = minecraft.gameDirectory.toPath().resolve("config").resolve("skycraft.properties");
		java.util.Properties props = new java.util.Properties();
		try {
			if (!java.nio.file.Files.exists(file)) {
				java.nio.file.Files.createDirectories(file.getParent());
				java.nio.file.Files.writeString(file, """
					# SkyCraft
					# To play in a friend's world instead of your own: put their address after join=
					# (the link e4mc shows them when they open their world to LAN), then restart Minecraft.
					join=
					""");
			}
			try (var in = java.nio.file.Files.newBufferedReader(file)) {
				props.load(in);
			}
		} catch (java.io.IOException e) {
			SkyCraft.LOG.warn("FNAFSBCraft: couldn't read {}", file, e);
			return null;
		}
		String join = props.getProperty("join", "").trim();
		return join.isEmpty() ? null : join;
	}

	/** /join: leave this world and play in a friend's (their e4mc link, or any server address). */
	public static void joinFriend(Minecraft minecraft, String link) {
		// People paste all sorts: "https://abc-def.e4mc.link/", " abc-def.e4mc.link ".
		String address = link.trim().replaceFirst("^[A-Za-z]+://", "").replaceAll("/+$", "");
		if (address.isEmpty()) {
			return;
		}
		SkyCraft.LOG.info("FNAFSBCraft: /join {}", address);
		sessionJoin = address;
		leaveWorld(minecraft);
	}

	/** The friend's world we're in (the address we joined), or null in our own. */
	public static @org.jspecify.annotations.Nullable String friendAddress(Minecraft minecraft) {
		if (sessionJoin != null) {
			return sessionJoin;
		}
		var server = minecraft.isLocalServer() ? null : minecraft.getCurrentServer();
		return server != null ? server.ip : null;
	}

	/** /leave: back to our own world. */
	public static void leaveFriend(Minecraft minecraft) {
		if (sessionJoin == null) {
			minecraft.gui.hud.getChat().addClientSystemMessage(net.minecraft.network.chat.Component.literal("You're already in your own world."));
			return;
		}
		SkyCraft.LOG.info("FNAFSBCraft: /leave {}", sessionJoin);
		sessionJoin = null;
		pendingNote = "Back in your own world.";
		leaveWorld(minecraft);
	}

	private static void leaveWorld(Minecraft minecraft) {
		attempted = false;
		minecraft.disconnectFromWorld(net.minecraft.client.multiplayer.ClientLevel.DEFAULT_QUIT_MESSAGE);
		minecraft.gui.setScreen(new TitleScreen());  // openWhenReady takes it from the title screen
	}

	/** Every client tick: a note for the player once they're in a world again. */
	public static void tick(Minecraft minecraft) {
		if (pendingNote != null && minecraft.player != null) {
			minecraft.gui.hud.getChat().addClientSystemMessage(net.minecraft.network.chat.Component.literal(pendingNote));
			pendingNote = null;
		}
		// The save was named while a world was still loading (so nothing was left then):
		// once it is open, to the save's own world (15 Oct: Steve never came).
		if (hostSave != null && minecraft.level != null && minecraft.player != null && sessionJoin == null && !wanted.equals(opened)) {
			SkyCraft.LOG.info("FNAFSBCraft: world {} is open but the save {} has {}, switching", opened, hostSave, wanted);
			leaveWorld(minecraft);
		}
	}

	// ---- a Minecraft world per Security Breach save ----
	// The host names the loaded save (FNAFSaveGameSystem.SaveSlotName). Each save gets a world of its
	// own, kept in fnafsbcraft_saves.txt ("save<TAB>world folder"). The world that was there before
	// (one shared by every save) goes to the first save loaded; New Game on a save starts a fresh
	// world for it (the old one is kept on disk).

	/** The world folder to be in; the one opened last. */
	private static String wanted = SkyCraft.WORLD_NAME;
	private static @org.jspecify.annotations.Nullable String opened;
	private static @org.jspecify.annotations.Nullable String hostSave;
	private static java.util.@org.jspecify.annotations.Nullable LinkedHashMap<String, String> saves;

	private static java.nio.file.Path savesFile() {
		return net.fabricmc.loader.api.FabricLoader.getInstance().getGameDir().resolve("fnafsbcraft_saves.txt");
	}

	private static java.util.LinkedHashMap<String, String> saves() {
		if (saves == null) {
			saves = new java.util.LinkedHashMap<>();
			try {
				if (java.nio.file.Files.exists(savesFile())) {
					for (String line : java.nio.file.Files.readAllLines(savesFile())) {
						int tab = line.indexOf('\t');
						if (tab > 0 && !line.startsWith("#")) {
							saves.put(line.substring(0, tab), line.substring(tab + 1));
						}
					}
				}
			} catch (Exception e) {
				SkyCraft.LOG.warn("FNAFSBCraft: couldn't read {}", savesFile(), e);
			}
		}
		return saves;
	}

	private static void writeSaves() {
		var lines = new java.util.ArrayList<String>();
		lines.add("# FNAFSBCraft: which Minecraft world belongs to which Security Breach save (save<TAB>world folder)");
		saves().forEach((save, world) -> lines.add(save + "\t" + world));
		try {
			java.nio.file.Files.write(savesFile(), lines);
		} catch (Exception e) {
			SkyCraft.LOG.warn("FNAFSBCraft: couldn't write {}", savesFile(), e);
		}
	}

	/**
	 * The play was saved into another slot: the open world goes on with it (the new slot gets
	 * it), and the slot before keeps a copy of how it is now. A world the new slot had before
	 * stays on disk.
	 */
	private static void savedInto(Minecraft minecraft, String before, String save) {
		var map = saves();
		String current = opened;
		String stem = SkyCraft.WORLD_NAME + " " + before.replaceAll("[^A-Za-z0-9_ -]", "_");
		String copy = stem;
		for (int n = 2; map.containsValue(copy) || minecraft.getLevelSource().levelExists(copy); n++) {
			copy = stem + " " + n;
		}
		map.put(save, current);
		map.put(before, copy);
		writeSaves();
		wanted = current;
		var server = minecraft.getSingleplayerServer();
		var from = minecraft.getLevelSource().getLevelPath(current);
		var to = minecraft.getLevelSource().getLevelPath(copy);
		final String copyName = copy;
		Runnable copyWorld = () -> {
			try (var files = java.nio.file.Files.walk(from)) {
				for (var file : (Iterable<java.nio.file.Path>) files::iterator) {
					var target = to.resolve(from.relativize(file).toString());
					if (java.nio.file.Files.isDirectory(file)) {
						java.nio.file.Files.createDirectories(target);
					} else if (!file.getFileName().toString().equals("session.lock")) {
						java.nio.file.Files.copy(file, target, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
					}
				}
				SkyCraft.LOG.info("FNAFSBCraft: saved into {}: world {} goes along, {} keeps a copy ({})", save, current, before, copyName);
			} catch (Exception e) {
				SkyCraft.LOG.warn("FNAFSBCraft: couldn't copy world {} for {}", current, before, e);
			}
		};
		if (server != null) {
			// Everything on disk first, then copied.
			server.execute(() -> {
				server.saveEverything(true, true, true);
				copyWorld.run();
			});
		} else {
			copyWorld.run();
		}
	}

	/** True for the mod's own worlds (no backup prompt for them). */
	public static boolean isOurs(String levelId) {
		return levelId.equals(SkyCraft.WORLD_NAME) || levelId.startsWith(SkyCraft.WORLD_NAME + " ");
	}

	/** True once the world of the save the host named is open (or no save was named yet). */
	public static boolean slotReady(Minecraft minecraft) {
		return hostSave == null || (minecraft.level != null && wanted.equals(opened));
	}

	/**
	 * The host loaded a save (how 0), started one with New Game (1), or saved the play into
	 * another slot (2): to its world.
	 */
	public static void hostSave(Minecraft minecraft, String save, int how) {
		boolean newGame = how == 1;
		var map = saves();
		String before = hostSave;
		boolean changed = !save.equals(hostSave) || newGame;
		hostSave = save;
		if (!changed) {
			return;
		}
		if (how == 2 && before != null && opened != null && minecraft.level != null) {
			savedInto(minecraft, before, save);
			return;
		}
		String world = map.get(save);
		if (newGame || world == null) {
			String current = opened != null ? opened : SkyCraft.WORLD_NAME;
			if (!newGame && !map.containsValue(current)) {
				// The world from before: it goes to the first save loaded.
				world = current;
			} else {
				String stem = SkyCraft.WORLD_NAME + " " + save.replaceAll("[^A-Za-z0-9_ -]", "_");
				world = stem;
				for (int n = 2; map.containsValue(world) || minecraft.getLevelSource().levelExists(world); n++) {
					world = stem + " " + n;
				}
			}
			map.put(save, world);
			writeSaves();
		}
		SkyCraft.LOG.info("FNAFSBCraft: Security Breach save {}{} -> world {}", save, newGame ? " (New Game)" : "", world);
		wanted = world;
		if (!world.equals(opened) && minecraft.level != null && sessionJoin == null) {
			SkyCraft.LOG.info("FNAFSBCraft: leaving world {} for {}", opened, world);
			leaveWorld(minecraft);
		}
	}

	public static void openWhenReady(Minecraft minecraft) {
		// Couldn't reach a friend's world, or it closed under us: back to our own, and say why.
		if (minecraft.gui.screen() instanceof net.minecraft.client.gui.screens.DisconnectedScreen && minecraft.level == null) {
			pendingNote = sessionJoin != null
				? "Couldn't stay in " + sessionJoin + " (check the link, and that your friend's world is still open to LAN). You're back in your own world."
				: "Disconnected. You're back in your own world.";
			SkyCraft.LOG.info("FNAFSBCraft: disconnected; back to the mirror world");
			sessionJoin = null;
			attempted = false;
			minecraft.gui.setScreen(new TitleScreen());
			return;
		}
		if (attempted && minecraft.level == null && minecraft.gui.screen() != null && System.currentTimeMillis() - lastLog > 5000) {
			lastLog = System.currentTimeMillis();
			SkyCraft.LOG.info("FNAFSBCraft: still not in the mirror world; current screen {}", minecraft.gui.screen().getClass().getName());
		}
		if (attempted || minecraft.level != null || minecraft.gui.overlay() != null) {
			return;
		}
		// Wait for the menu to settle on the title screen; skip any first-launch prompts in front of it.
		if (!(minecraft.gui.screen() instanceof TitleScreen)) {
			if (minecraft.gui.screen() != null && System.currentTimeMillis() - lastLog > 5000) {
				lastLog = System.currentTimeMillis();
				SkyCraft.LOG.info("FNAFSBCraft: waiting on screen {} before opening the mirror world", minecraft.gui.screen().getClass().getName());
			}
			if (minecraft.gui.screen() == null || minecraft.gui.screen().getClass().getName().contains("Onboarding")) {
				minecraft.gui.setScreen(new TitleScreen());
			}
			return;
		}
		TitleScreen title = (TitleScreen) minecraft.gui.screen();
		attempted = true;
		// Multiplayer: join a friend's world (their e4mc link, or any server address) instead.
		String join = sessionJoin != null ? sessionJoin : joinAddress(minecraft);
		if (join != null) {
			SkyCraft.LOG.info("FNAFSBCraft: joining {}", join);
			pendingNote = "Joined " + join + ". Type /leave to go back to your own world.";
			net.minecraft.client.gui.screens.ConnectScreen.startConnecting(title, minecraft, net.minecraft.client.multiplayer.resolver.ServerAddress.parseString(join),
				new net.minecraft.client.multiplayer.ServerData("FNAFSBCraft", join, net.minecraft.client.multiplayer.ServerData.Type.OTHER), false, null);
			return;
		}
		opened = wanted;
		if (minecraft.getLevelSource().levelExists(wanted)) {
			SkyCraft.LOG.info("FNAFSBCraft: opening mirror world {}", wanted);
			minecraft.createWorldOpenFlows().openWorld(wanted, () -> minecraft.gui.setScreen(title));
			return;
		}
		SkyCraft.LOG.info("FNAFSBCraft: creating mirror world {}", wanted);
		LevelSettings settings = new LevelSettings(
			wanted,
			GameType.SURVIVAL,
			new LevelSettings.DifficultySettings(Difficulty.NORMAL, false, false),
			true,
			WorldDataConfiguration.DEFAULT
		);
		minecraft.createWorldOpenFlows().createFreshLevel(
			wanted,
			settings,
			new WorldOptions(0L, false, false),
			registries -> registries.lookupOrThrow(Registries.WORLD_PRESET).getOrThrow(PRESET).value().createWorldDimensions(),
			title
		);
	}
}
