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
	/**
	 * The tree, by achievement id: parent, icon, frame. Five branches from the root: the story
	 * (escaping each area to the endings), the shattered animatronics, the side attractions,
	 * collecting, and surviving.
	 */
	private static final Map<String, String[]> TREE = Map.ofEntries(
		// The story.
		Map.entry("ach_sleepover", new String[] { "root", "minecraft:red_bed", "task" }),
		Map.entry("ach_wub", new String[] { "ach_sleepover", "minecraft:jukebox", "task" }),
		Map.entry("ach_hazard", new String[] { "ach_wub", "minecraft:minecart", "task" }),
		Map.entry("ach_alarm", new String[] { "ach_hazard", "minecraft:bell", "task" }),
		Map.entry("ach_security", new String[] { "ach_alarm", "minecraft:tripwire_hook", "goal" }),
		Map.entry("ach_superstar", new String[] { "ach_security", "minecraft:clock", "goal" }),
		Map.entry("ach_fun", new String[] { "ach_superstar", "minecraft:nether_star", "challenge" }),
		Map.entry("ach_me", new String[] { "ach_security", "minecraft:redstone", "task" }),
		// The animatronics.
		Map.entry("ach_speak", new String[] { "root", "minecraft:cake", "task" }),
		Map.entry("ach_claw", new String[] { "ach_speak", "minecraft:turtle_helmet", "task" }),
		Map.entry("ach_see", new String[] { "ach_claw", "minecraft:bone", "task" }),
		Map.entry("ach_shattered", new String[] { "ach_see", "minecraft:anvil", "challenge" }),
		// Side attractions.
		Map.entry("ach_dessert", new String[] { "root", "minecraft:cookie", "task" }),
		Map.entry("ach_pizza", new String[] { "ach_dessert", "minecraft:bread", "task" }),
		Map.entry("ach_opa", new String[] { "ach_dessert", "minecraft:bowl", "task" }),
		Map.entry("ach_party", new String[] { "root", "minecraft:paper", "task" }),
		Map.entry("ach_lazer", new String[] { "ach_party", "minecraft:crossbow", "goal" }),
		Map.entry("ach_heckler", new String[] { "ach_lazer", "minecraft:lightning_rod", "task" }),
		Map.entry("ach_par", new String[] { "ach_party", "minecraft:slime_ball", "task" }),
		Map.entry("ach_sugar", new String[] { "ach_party", "minecraft:sugar", "task" }),
		Map.entry("ach_cheese", new String[] { "ach_party", "minecraft:glow_ink_sac", "task" }),
		// Collecting.
		Map.entry("ach_found", new String[] { "root", "minecraft:writable_book", "goal" }),
		Map.entry("ach_sword", new String[] { "ach_found", "minecraft:iron_sword", "task" }),
		Map.entry("ach_up", new String[] { "ach_found", "minecraft:firework_rocket", "task" }),
		Map.entry("ach_vip", new String[] { "root", "minecraft:gold_ingot", "challenge" }),
		Map.entry("ach_trophy", new String[] { "ach_vip", "minecraft:totem_of_undying", "challenge" }),
		// Surviving.
		Map.entry("ach_surprise", new String[] { "root", "minecraft:creeper_head", "task" }),
		Map.entry("ach_dodge", new String[] { "ach_surprise", "minecraft:feather", "task" }),
		Map.entry("ach_hide", new String[] { "ach_dodge", "minecraft:ender_eye", "challenge" }));

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
				    "background": "minecraft:block/black_concrete",
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
				// Where it hangs in the tree, its icon and frame (unknown ones: under the root).
				String[] place = TREE.getOrDefault(path, new String[] { "root", ICONS[index % ICONS.length], "task" });
				String parent = place[0].equals("root") || LIST.values().stream().anyMatch(f -> pathOf(f[0]).equals(place[0])) ? place[0] : "root";
				String body = "{\n"
					+ "  \"parent\": \"skycraft:security_breach/" + parent + "\",\n"
					+ "  \"criteria\": { \"unlocked\": { \"trigger\": \"minecraft:impossible\" } },\n"
					+ "  \"display\": {\n"
					+ "    \"title\": " + json(fields[1].isEmpty() ? fields[0] : fields[1]) + ",\n"
					+ "    \"description\": " + json(fields[2]) + ",\n"
					+ "    \"icon\": { \"id\": \"" + place[1] + "\" },\n"
					+ "    \"frame\": \"" + place[2] + "\",\n"
					// Ones the player had already don't pop up all at once.
					+ "    \"show_toast\": " + !had + ",\n"
					+ "    \"announce_to_chat\": false,\n"
					// Steam hides them all; here the tree would stay empty, so they show.
					+ "    \"hidden\": false\n"
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
