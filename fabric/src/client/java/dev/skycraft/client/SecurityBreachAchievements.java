package dev.skycraft.client;

import dev.skycraft.SkyCraft;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import net.minecraft.client.Minecraft;
import net.minecraft.server.MinecraftServer;
import net.minecraft.world.level.storage.LevelResource;

/**
 * Security Breach's own achievements as a tab of Minecraft advancements. The host sends their ids,
 * names and descriptions once, and later which ones the player unlocked. They go into a data pack
 * in the world (written once, or again when they changed), and an unlocked one is granted: its
 * toast pops like any advancement's. Ones the player already had when the pack was written are
 * granted quietly.
 */
final class SecurityBreachAchievements {
	private SecurityBreachAchievements() {
	}

	private static final String PACK = "security_breach_achievements";
	private static final Map<Integer, StringBuilder> TEXT = new HashMap<>();
	private static final Map<Integer, String[]> LIST = new HashMap<>();
	private static final Map<Integer, Boolean> HIDDEN = new HashMap<>();
	private static final Set<Integer> HAD = new HashSet<>();
	private static final Set<String> TO_GRANT = new HashSet<>();
	private static boolean written;
	private static int ticks;

	/** Twelve more characters of one field of one achievement. */
	static void text(int code, int a, int b, int c) {
		var text = TEXT.computeIfAbsent(code, k -> new StringBuilder());
		for (int word : new int[] { a, b, c }) {
			for (int i = 0; i < 4; i++) {
				int ch = (word >>> (i * 8)) & 0xFF;
				if (ch != 0) {
					text.append((char) ch);
				}
			}
		}
	}

	/** Achievement `index` is complete (and whether the player has it); the last one writes the pack. */
	static void done(Minecraft minecraft, int index, boolean unlocked, boolean hidden, int total) {
		String[] fields = LIST.get(index);
		if (fields == null || TEXT.containsKey(index * 4)) {
			fields = new String[3];
			for (int f = 0; f < 3; f++) {
				var t = TEXT.remove(index * 4 + f);
				fields[f] = t == null ? "" : t.toString();
			}
			LIST.put(index, fields);
			HIDDEN.put(index, hidden);
			if (unlocked) {
				HAD.add(index);
			}
		} else if (unlocked) {
			// Unlocked while playing: the toast.
			TO_GRANT.add(pathOf(fields[0]));
		}
		if (!written && LIST.size() >= total) {
			var server = minecraft.getSingleplayerServer();
			if (server != null) {
				server.execute(() -> write(server));
			}
		}
	}

	/** Grants what waits, once the pack is loaded (a reload takes a moment). */
	static void tick(Minecraft minecraft) {
		if (TO_GRANT.isEmpty() || ++ticks % 40 != 0) {
			return;
		}
		var server = minecraft.getSingleplayerServer();
		if (server == null) {
			return;
		}
		Set<String> grant = new HashSet<>(TO_GRANT);
		server.execute(() -> {
			for (String path : grant) {
				var id = net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "security_breach/" + path);
				if (server.getAdvancements().get(id) == null) {
					continue; // not loaded yet
				}
				server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(),
					"advancement grant @a only skycraft:security_breach/" + path);
				TO_GRANT.remove(path);
				SkyCraft.LOG.info("FNAFSBCraft: Security Breach achievement {} granted", path);
			}
		});
	}

	private static String pathOf(String id) {
		String path = id.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9_]", "_");
		return path.isEmpty() ? "unnamed" : path;
	}

	private static String json(String text) {
		StringBuilder out = new StringBuilder("\"");
		for (char c : text.toCharArray()) {
			switch (c) {
				case '"' -> out.append("\\\"");
				case '\\' -> out.append("\\\\");
				case '\n' -> out.append("\\n");
				case '\r' -> {
				}
				default -> out.append(c < 0x20 ? ' ' : c);
			}
		}
		return out.append('"').toString();
	}

	// Icons: the achievements are many and alike, so a few Pizzaplex-ish items in turn.
	private static final String[] ICONS = {
		"minecraft:clock", "minecraft:cake", "minecraft:music_disc_cat", "minecraft:redstone_torch", "minecraft:jack_o_lantern",
		"minecraft:goat_horn", "minecraft:firework_rocket", "minecraft:golden_carrot", "minecraft:jukebox", "minecraft:lantern"
	};

	private static void write(MinecraftServer server) {
		if (written) {
			return;
		}
		written = true;
		try {
			Path pack = server.getWorldPath(LevelResource.DATAPACK_DIR).resolve(PACK);
			Path dir = pack.resolve("data/skycraft/advancement/security_breach");
			Files.createDirectories(dir);
			boolean changed = writeIfChanged(pack.resolve("pack.mcmeta"), """
				{
				  "pack": {
				    "description": "Five Nights at Freddy's: Security Breach achievements",
				    "min_format": 121,
				    "max_format": 121
				  }
				}
				""");
			changed |= writeIfChanged(dir.resolve("root.json"), """
				{
				  "criteria": { "joined": { "trigger": "minecraft:tick" } },
				  "display": {
				    "title": "Security Breach",
				    "description": "Survive the night at Freddy Fazbear's Mega Pizzaplex",
				    "icon": { "id": "minecraft:clock" },
				    "background": "minecraft:gui/advancements/backgrounds/stone",
				    "show_toast": false,
				    "announce_to_chat": false
				  }
				}
				""");
			for (var entry : LIST.entrySet()) {
				int index = entry.getKey();
				String[] fields = entry.getValue();
				String path = pathOf(fields[0]);
				boolean had = HAD.contains(index);
				String body = "{\n"
					+ "  \"parent\": \"skycraft:security_breach/root\",\n"
					+ "  \"criteria\": { \"unlocked\": { \"trigger\": \"minecraft:impossible\" } },\n"
					+ "  \"display\": {\n"
					+ "    \"title\": " + json(fields[1].isEmpty() ? fields[0] : fields[1]) + ",\n"
					+ "    \"description\": " + json(fields[2]) + ",\n"
					+ "    \"icon\": { \"id\": \"" + ICONS[index % ICONS.length] + "\" },\n"
					+ "    \"frame\": \"challenge\",\n"
					// Ones the player had already don't pop up all at once.
					+ "    \"show_toast\": " + !had + ",\n"
					+ "    \"announce_to_chat\": false,\n"
					+ "    \"hidden\": " + HIDDEN.getOrDefault(index, false) + "\n"
					+ "  }\n"
					+ "}\n";
				changed |= writeIfChanged(dir.resolve(path + ".json"), body);
				if (had) {
					TO_GRANT.add(path);
				}
			}
			SkyCraft.LOG.info("FNAFSBCraft: {} Security Breach achievements in the data pack ({})", LIST.size(), changed ? "written, reloading" : "unchanged");
			if (changed) {
				server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "reload");
			}
		} catch (Exception e) {
			SkyCraft.LOG.warn("FNAFSBCraft: couldn't write the Security Breach achievements", e);
		}
	}

	private static boolean writeIfChanged(Path file, String content) throws java.io.IOException {
		if (Files.exists(file) && Files.readString(file, StandardCharsets.UTF_8).equals(content)) {
			return false;
		}
		Files.writeString(file, content, StandardCharsets.UTF_8);
		return true;
	}
}
