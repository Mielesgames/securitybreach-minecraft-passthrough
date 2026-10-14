package dev.skycraft;

import dev.skycraft.combat.SkyCombat;
import net.fabricmc.api.ModInitializer;
import net.minecraft.world.entity.EquipmentSlot;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.gamerules.GameRules;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class SkyCraft implements ModInitializer {
	public static final String MOD_ID = "skycraft";
	public static final String WORLD_NAME = "SkyCraft";
	public static final Logger LOG = LoggerFactory.getLogger(MOD_ID);
	private static final String KIT2_TAG = "skycraft_builder_kit";
	private static final String KIT3_TAG = "skycraft_pizzaplex_kit";
	private static final String KIT4_TAG = "skycraft_iron_pickaxe";

	/** Security Breach: Steve may open (and redstone may open) the game's doors. */
	public static final net.minecraft.world.level.gamerules.GameRule<Boolean> ALLOW_OPENING_DOORS =
		net.fabricmc.fabric.api.gamerule.v1.GameRuleBuilder.forBoolean(true)
			.category(net.minecraft.world.level.gamerules.GameRuleCategory.PLAYER)
			.buildAndRegister(net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "allow_opening_doors"));

	/** Security Breach: a hole in the host's world (see InvertedBlock). */
	public static final net.minecraft.resources.ResourceKey<net.minecraft.world.level.block.Block> INVERTED_BLOCK_KEY = net.minecraft.resources.ResourceKey.create(
		net.minecraft.core.registries.Registries.BLOCK, net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "inverted_block"));
	public static final net.minecraft.world.level.block.Block INVERTED_BLOCK = net.minecraft.core.Registry.register(
		net.minecraft.core.registries.BuiltInRegistries.BLOCK, INVERTED_BLOCK_KEY,
		new dev.skycraft.world.InvertedBlock(net.minecraft.world.level.block.state.BlockBehaviour.Properties.of()
			.setId(INVERTED_BLOCK_KEY).noCollision().noOcclusion().strength(0.3F)));
	public static final net.minecraft.resources.ResourceKey<net.minecraft.world.item.Item> INVERTED_ITEM_KEY = net.minecraft.resources.ResourceKey.create(
		net.minecraft.core.registries.Registries.ITEM, net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "inverted_block"));
	public static final net.minecraft.world.item.Item INVERTED_ITEM = net.minecraft.core.Registry.register(
		net.minecraft.core.registries.BuiltInRegistries.ITEM, INVERTED_ITEM_KEY,
		new net.minecraft.world.item.BlockItem(INVERTED_BLOCK, new net.minecraft.world.item.Item.Properties().setId(INVERTED_ITEM_KEY).useBlockDescriptionPrefix()));

	/** Security Breach: fills a hole mined into the host's world again. */
	public static final net.minecraft.resources.ResourceKey<net.minecraft.world.item.Item> VOID_FILLER_KEY = net.minecraft.resources.ResourceKey.create(
		net.minecraft.core.registries.Registries.ITEM, net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "void_filler"));
	public static final net.minecraft.world.item.Item VOID_FILLER = net.minecraft.core.Registry.register(
		net.minecraft.core.registries.BuiltInRegistries.ITEM, VOID_FILLER_KEY,
		new net.minecraft.world.item.Item(new net.minecraft.world.item.Item.Properties().setId(VOID_FILLER_KEY).stacksTo(1)));

	/** Security Breach: a water bucket scoops up an animatronic, like a fish. */
	public static final net.minecraft.world.level.gamerules.GameRule<Boolean> ALLOW_ANIMATRONIC_WATER_BUCKET =
		net.fabricmc.fabric.api.gamerule.v1.GameRuleBuilder.forBoolean(true)
			.category(net.minecraft.world.level.gamerules.GameRuleCategory.PLAYER)
			.buildAndRegister(net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "allow_animatronic_water_bucket"));

	/** The bucket a scooped-up Security Breach character is in: its name, and the host's number for it. */
	public static net.minecraft.world.item.ItemStack tokenBucket(int id, String name) {
		var bucket = new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.WATER_BUCKET);
		bucket.set(net.minecraft.core.component.DataComponents.CUSTOM_NAME, net.minecraft.network.chat.Component.literal("Bucket of " + name));
		picture(bucket, "bucket_" + castKey(name));
		bucket.set(net.minecraft.core.component.DataComponents.ENCHANTMENT_GLINT_OVERRIDE, true);
		var tag = new net.minecraft.nbt.CompoundTag();
		tag.putInt(TOKEN_KEY, id);
		bucket.set(net.minecraft.core.component.DataComponents.CUSTOM_DATA, net.minecraft.world.item.component.CustomData.of(tag));
		return bucket;
	}

	/** The Pizzaplex's areas for /tp, in the order the host knows them (sb's kZones). */
	public static final String[] ZONES = {
		"kitchen", "atrium", "east_arcade", "west_arcade", "roxy_raceway", "gator_golf", "daycare", "utilidoors", "pizza_sim", "fazer_blast"
	};

	/** The cast as the chat and the items name them, by command name. */
	private static final java.util.Map<String, String> CAST_NAMES = java.util.Map.ofEntries(
		java.util.Map.entry("freddy", "Glamrock Freddy"), java.util.Map.entry("chica", "Glamrock Chica"),
		java.util.Map.entry("monty", "Montgomery Gator"), java.util.Map.entry("roxy", "Roxanne Wolf"),
		java.util.Map.entry("vanny", "Vanny"), java.util.Map.entry("vanessa", "Vanessa"), java.util.Map.entry("moon", "Moon"),
		java.util.Map.entry("sun", "Sun"), java.util.Map.entry("bot", "Staff Bot"), java.util.Map.entry("endo", "Endo"),
		java.util.Map.entry("musicman", "DJ Music Man"));
	private static final String SUMMON_KEY = "skycraft_summon";

	/**
	 * A spawn egg (or bucket) from the Security Breach creative tab: using it summons that character in
	 * front of the player, like "/summon chica". Paper underneath (a real spawn egg would hatch its
	 * own mob if this ever missed), with the egg's picture.
	 */
	public static net.minecraft.world.item.ItemStack summonItem(String name, boolean bucket) {
		var stack = new net.minecraft.world.item.ItemStack(bucket ? net.minecraft.world.item.Items.WATER_BUCKET : net.minecraft.world.item.Items.PAPER);
		String shown = CAST_NAMES.getOrDefault(name, name);
		stack.set(net.minecraft.core.component.DataComponents.CUSTOM_NAME, net.minecraft.network.chat.Component.literal(bucket ? "Bucket of " + shown : shown + " Spawn Egg"));
		picture(stack, (bucket ? "bucket_" : "egg_") + castKey(name));
		var tag = new net.minecraft.nbt.CompoundTag();
		tag.putString(SUMMON_KEY, name);
		stack.set(net.minecraft.core.component.DataComponents.CUSTOM_DATA, net.minecraft.world.item.component.CustomData.of(tag));
		return stack;
	}

	private static String summonOf(net.minecraft.world.item.ItemStack stack) {
		var data = stack.get(net.minecraft.core.component.DataComponents.CUSTOM_DATA);
		return data == null ? "" : data.copyTag().getStringOr(SUMMON_KEY, "");
	}

	/** Uses a summoning egg or bucket: the host makes that character; true if it was one. */
	private static boolean useSummon(net.minecraft.world.entity.player.Player player, net.minecraft.world.InteractionHand hand) {
		var stack = player.getItemInHand(hand);
		String name = summonOf(stack);
		if (name.isEmpty()) {
			return false;
		}
		if (!player.level().isClientSide()) {
			int id = java.util.Arrays.asList(dev.skycraft.link.Proto.HOST_CHARACTERS).indexOf(name) + 1;
			if (id > 0 && dev.skycraft.link.SkyLink.active()) {
				dev.skycraft.link.SkyLink.pushEvent(dev.skycraft.link.Proto.EV_COMMAND, id, 0, 0, 0, 0, 1);
				LOG.info("FNAFSBCraft: {} summoned from {}", name, stack.getHoverName().getString());
			}
			if (!player.getAbilities().instabuild) {
				boolean bucket = stack.is(net.minecraft.world.item.Items.WATER_BUCKET);
				stack.shrink(1);
				if (bucket) {
					player.getInventory().add(new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.WATER_BUCKET));
				}
			}
		}
		return true;
	}

	/** Which of the mod's pictures fits a Security Breach character's name ("ChicaStandard" -> chica). */
	private static String castKey(String name) {
		String lower = name.toLowerCase(java.util.Locale.ROOT);
		for (String key : new String[] { "vanessa", "vanny", "musicman", "freddy", "chica", "roxy", "monty", "moon", "sun", "endo", "bot" }) {
			if (lower.contains(key)) {
				return key;
			}
		}
		return "animatronic";
	}

	private static void picture(net.minecraft.world.item.ItemStack stack, String model) {
		stack.set(net.minecraft.core.component.DataComponents.ITEM_MODEL, net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", model));
	}

	/** The spawn egg of a Security Breach character (creative pick): its name, and the host's number for it. */
	public static net.minecraft.world.item.ItemStack tokenEgg(int id, String name) {
		var egg = new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.POLAR_BEAR_SPAWN_EGG);
		egg.set(net.minecraft.core.component.DataComponents.CUSTOM_NAME, net.minecraft.network.chat.Component.literal(name + " Spawn Egg"));
		picture(egg, "egg_" + castKey(name));
		var tag = new net.minecraft.nbt.CompoundTag();
		tag.putInt(TOKEN_KEY, id);
		egg.set(net.minecraft.core.component.DataComponents.CUSTOM_DATA, net.minecraft.world.item.component.CustomData.of(tag));
		return egg;
	}

	/** The paper a broken Security Breach object leaves: its name, and the host's number for it. */
	public static net.minecraft.world.item.ItemStack tokenPaper(int id, String name) {
		var paper = new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.PAPER);
		paper.set(net.minecraft.core.component.DataComponents.CUSTOM_NAME, net.minecraft.network.chat.Component.literal(name));
		picture(paper, "sb_object");
		var tag = new net.minecraft.nbt.CompoundTag();
		tag.putInt(TOKEN_KEY, id);
		paper.set(net.minecraft.core.component.DataComponents.CUSTOM_DATA, net.minecraft.world.item.component.CustomData.of(tag));
		return paper;
	}

	public static final String TOKEN_KEY = "skycraft_token";

	/** The host's number on a broken-object paper, or 0. */
	public static int tokenOf(net.minecraft.world.item.ItemStack stack) {
		var data = stack.get(net.minecraft.core.component.DataComponents.CUSTOM_DATA);
		return data == null ? 0 : data.copyTag().getIntOr(TOKEN_KEY, 0);
	}

	private static int hostCommand(net.minecraft.commands.CommandSourceStack source, String name, boolean spawn, boolean jockey) {
		if (jockey) {
			dev.skycraft.combat.SkyCombat.forceJockey(name, spawn);
		}
		return hostCommand(source, name, spawn);
	}

	private static int hostCommand(net.minecraft.commands.CommandSourceStack source, String name, boolean spawn) {
		if (!dev.skycraft.link.SkyLink.active()) {
			source.sendFailure(net.minecraft.network.chat.Component.literal("Not linked to the game."));
			return 0;
		}
		int id = java.util.Arrays.asList(dev.skycraft.link.Proto.HOST_CHARACTERS).indexOf(name) + 1;
		dev.skycraft.link.SkyLink.pushEvent(dev.skycraft.link.Proto.EV_COMMAND, id, 0, 0, 0, 0, spawn ? 1 : 0);
		source.sendSuccess(() -> net.minecraft.network.chat.Component.literal(spawn ? "Summoning " + name + "." : name + " is on the way."), false);
		return 1;
	}

	@Override
	public void onInitialize() {
		SkyCombat.init();
		// (The inverted block stays registered for worlds that have one; holes are mined now.)
		net.fabricmc.fabric.api.creativetab.v1.CreativeModeTabEvents.modifyOutputEvent(net.minecraft.world.item.CreativeModeTabs.TOOLS_AND_UTILITIES)
			.register(output -> output.accept(VOID_FILLER));
		// The Security Breach tab: a spawn egg and a bucket for every character, and the mod's tools.
		net.minecraft.core.Registry.register(net.minecraft.core.registries.BuiltInRegistries.CREATIVE_MODE_TAB,
			net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "security_breach"),
			net.fabricmc.fabric.api.creativetab.v1.FabricCreativeModeTab.builder()
				.title(net.minecraft.network.chat.Component.literal("Security Breach"))
				.icon(() -> summonItem("freddy", false))
				.displayItems((parameters, output) -> {
					for (String name : dev.skycraft.link.Proto.HOST_CHARACTERS) {
						output.accept(summonItem(name, false));
					}
					for (String name : dev.skycraft.link.Proto.HOST_CHARACTERS) {
						output.accept(summonItem(name, true));
					}
					output.accept(new net.minecraft.world.item.ItemStack(VOID_FILLER));
					output.accept(new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.CARVED_PUMPKIN));
					output.accept(new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.LEAD));
					output.accept(new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.ELYTRA));
					output.accept(new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.SHEARS));
				})
				.build());
		// Using a summoning egg or bucket, in the air or on a block.
		net.fabricmc.fabric.api.event.player.UseItemCallback.EVENT.register((player, level, hand) ->
			useSummon(player, hand) ? net.minecraft.world.InteractionResult.SUCCESS : net.minecraft.world.InteractionResult.PASS);
		net.fabricmc.fabric.api.event.player.UseBlockCallback.EVENT.register((player, level, hand, hit) ->
			useSummon(player, hand) ? net.minecraft.world.InteractionResult.SUCCESS : net.minecraft.world.InteractionResult.PASS);
		// A Void Filler on a hole mined into Security Breach: its floor or wall comes back.
		net.fabricmc.fabric.api.event.player.UseBlockCallback.EVENT.register((player, level, hand, hit) -> {
			if (!player.getItemInHand(hand).is(VOID_FILLER)) {
				return net.minecraft.world.InteractionResult.PASS;
			}
			if (!level.isClientSide() && level instanceof net.minecraft.server.level.ServerLevel server) {
				int world = dev.skycraft.link.SkyLink.readSkyStateWorld();
				var pos = hit.getBlockPos();
				boolean filled = false;
				for (var cell : new net.minecraft.core.BlockPos[] { pos.relative(hit.getDirection()), pos, pos.above() }) {
					if (server.getBlockState(cell).is(INVERTED_BLOCK)) {
						server.setBlock(cell, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 3);
						filled = true;
						break;
					}
					if (dev.skycraft.world.SkyDig.fill(server, world, cell)) {
						filled = true;
						break;
					}
				}
				LOG.info("FNAFSBCraft: void filler at {}: {}", pos, filled ? "a hole filled" : "no hole there");
			}
			return net.minecraft.world.InteractionResult.SUCCESS;
		});
		// Digging deeper: under a broken foundation block there is ground again.
		net.fabricmc.fabric.api.event.player.PlayerBlockBreakEvents.AFTER.register((level, player, pos, state, blockEntity) -> {
			if (dev.skycraft.world.SkyDig.PASSTHROUGH && state.is(dev.skycraft.world.SkyDig.FOUNDATION) && level instanceof net.minecraft.server.level.ServerLevel server) {
				// The cell is part of the hole now (drawn as one, walled like one), with ground under it.
				dev.skycraft.world.SkyDig.markDug(server, dev.skycraft.link.SkyLink.readSkyStateWorld(), pos);
				dev.skycraft.world.SkyDig.underground(server, pos);
			}
		});
		LOG.info("FNAFSBCraft: inverted block registered as {}", INVERTED_BLOCK_KEY.identifier());
		// An inverted block put against the host's floor or wall goes into it (that is where the hole
		// is wanted), not in front of it like a normal block. The host's surfaces aren't blocks: the
		// cell that was clicked is air, so the one behind the clicked face is the surface's own.
		net.fabricmc.fabric.api.event.player.UseBlockCallback.EVENT.register((player, level, hand, hit) -> {
			var stack = player.getItemInHand(hand);
			if (!stack.is(INVERTED_ITEM) || !level.getBlockState(hit.getBlockPos()).isAir()) {
				return net.minecraft.world.InteractionResult.PASS;
			}
			var into = hit.getBlockPos().relative(hit.getDirection().getOpposite());
			if (!level.getBlockState(into).isAir()) {
				return net.minecraft.world.InteractionResult.PASS;
			}
			if (!level.isClientSide()) {
				level.setBlock(into, INVERTED_BLOCK.defaultBlockState(), 3);
				if (!player.getAbilities().instabuild) {
					stack.shrink(1);
				}
				LOG.info("FNAFSBCraft: inverted block put into the host's surface at {}", into);
			}
			return net.minecraft.world.InteractionResult.SUCCESS;
		});
		dev.skycraft.net.SkyNet.init();
		dev.skycraft.world.SkyDig.init();
		// Commands for the host game (Security Breach): "/freddy" or "/tp freddy" brings that
		// character to the player, "/summon freddy" makes a new one. One per character. They are
		// extra branches of Minecraft's own /tp and /summon, so the chat completes them.
		net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
			// "/tp atrium": to one of the Pizzaplex's areas (the game's own room areas; the host
			// puts Gregory in one of that area's rooms). Numbers 100 and up are zones.
			for (int i = 0; i < ZONES.length; i++) {
				final int zone = i;
				dispatcher.register(net.minecraft.commands.Commands.literal("teleport")
					.then(net.minecraft.commands.Commands.literal(ZONES[i]).executes(context -> {
						if (!dev.skycraft.link.SkyLink.active()) {
							context.getSource().sendFailure(net.minecraft.network.chat.Component.literal("Not linked to the game."));
							return 0;
						}
						dev.skycraft.link.SkyLink.pushEvent(dev.skycraft.link.Proto.EV_COMMAND, 100 + zone, 0, 0, 0, 0, 0);
						context.getSource().sendSuccess(() -> net.minecraft.network.chat.Component.literal("Going to " + ZONES[zone].replace('_', ' ') + "."), false);
						return 1;
					})));
			}
			// /repairworld: every hole in Security Breach near the players closed again.
			dispatcher.register(net.minecraft.commands.Commands.literal("repairworld").executes(context -> {
				var level = context.getSource().getLevel();
				int filled = dev.skycraft.world.SkyDig.repairAll(level, 12);
				LOG.info("FNAFSBCraft: /repairworld filled {} holes", filled);
				context.getSource().sendSuccess(() -> net.minecraft.network.chat.Component.literal(
					filled == 0 ? "No holes nearby." : "Repaired " + filled + " hole" + (filled == 1 ? "" : "s") + " in the Pizzaplex."), false);
				return filled;
			}));
			// /clearworld confirm: everything of Minecraft's near the players gone, as in a new save.
			dispatcher.register(net.minecraft.commands.Commands.literal("clearworld")
				.executes(context -> {
					context.getSource().sendSuccess(() -> net.minecraft.network.chat.Component.literal(
						"This removes every Minecraft block, mob, item and hole near you. Type /clearworld confirm to do it."), false);
					return 0;
				})
				.then(net.minecraft.commands.Commands.literal("confirm").executes(context -> {
					int removed = clearWorld(context.getSource().getLevel(), 12);
					context.getSource().sendSuccess(() -> net.minecraft.network.chat.Component.literal(
						"Cleared: " + removed + " blocks and things. The Pizzaplex is as it was."), false);
					return removed;
				})));
			for (String name : dev.skycraft.link.Proto.HOST_CHARACTERS) {
				// "... jockey": it rides a chicken.
				dispatcher.register(net.minecraft.commands.Commands.literal(name).executes(context -> hostCommand(context.getSource(), name, false))
					.then(net.minecraft.commands.Commands.literal("jockey").executes(context -> hostCommand(context.getSource(), name, false, true))));
				// "tp" only forwards to "teleport": a branch added there is found by both.
				dispatcher.register(net.minecraft.commands.Commands.literal("teleport")
					.then(net.minecraft.commands.Commands.literal(name).executes(context -> hostCommand(context.getSource(), name, false))
						.then(net.minecraft.commands.Commands.literal("jockey").executes(context -> hostCommand(context.getSource(), name, false, true)))));
				dispatcher.register(net.minecraft.commands.Commands.literal("summon")
					.then(net.minecraft.commands.Commands.literal(name).executes(context -> hostCommand(context.getSource(), name, true))
						.then(net.minecraft.commands.Commands.literal("jockey").executes(context -> hostCommand(context.getSource(), name, true, true)))));
			}
		});
		ServerLifecycleEvents.SERVER_STARTED.register(SkyCraft::configureServer);
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			// A player who quit on Freddy's mule or on a hiding seat comes back sitting on it, with a
			// horse's jump bar and no legs of their own. Off it, once the vehicle has loaded too.
			var joined = handler.getPlayer();
			server.execute(() -> {
				var vehicle = joined.getVehicle();
				if (vehicle != null) {
					joined.stopRiding();
					if (vehicle instanceof net.minecraft.world.entity.decoration.ArmorStand) {
						vehicle.discard();
					}
					LOG.info("FNAFSBCraft: {} joined riding {}, taken off it", joined.getName().getString(), vehicle.getName().getString());
				}
			});
			// An empty inventory at spawn is filled again: every kit, as if for the first time.
			if (handler.getPlayer().getInventory().isEmpty()) {
				handler.getPlayer().removeTag(KIT2_TAG);
				handler.getPlayer().removeTag(KIT3_TAG);
				LOG.info("FNAFSBCraft: {} spawned with nothing, the kits are given again", handler.getPlayer().getName().getString());
			}
			giveStarterKit(handler.getPlayer());
			giveBuilderKit(handler.getPlayer());
			givePizzaplexKit(handler.getPlayer());
			server.execute(() -> fixPizzaplexKit(joined));
			dressTestGuest(handler.getPlayer());
		});
	}

	/**
	 * /clearworld confirm: within `chunks` chunks of the players, every Minecraft block goes (fast, no
	 * neighbour updates: nothing falls or drops), every entity but the players goes (mobs, items,
	 * Freddy's mule, the helpers; the stand-ins come back by themselves), and every hole is closed.
	 * The players and what they carry stay.
	 */
	private static int clearWorld(net.minecraft.server.level.ServerLevel level, int chunks) {
		int removed = dev.skycraft.world.SkyDig.repairAll(level, chunks);
		java.util.Set<Long> done = new java.util.HashSet<>();
		var air = net.minecraft.world.level.block.Blocks.AIR.defaultBlockState();
		for (var player : level.players()) {
			int cx = player.blockPosition().getX() >> 4, cz = player.blockPosition().getZ() >> 4;
			for (int x = cx - chunks; x <= cx + chunks; x++) {
				for (int z = cz - chunks; z <= cz + chunks; z++) {
					if (!done.add(net.minecraft.world.level.ChunkPos.pack(x, z)) || !level.hasChunk(x, z)) {
						continue;
					}
					var chunk = level.getChunk(x, z);
					var sections = chunk.getSections();
					for (int i = 0; i < sections.length; i++) {
						if (sections[i] == null || sections[i].hasOnlyAir()) {
							continue;
						}
						int baseY = chunk.getSectionYFromSectionIndex(i) << 4;
						for (int y = 0; y < 16; y++) {
							for (int zz = 0; zz < 16; zz++) {
								for (int xx = 0; xx < 16; xx++) {
									if (!sections[i].getBlockState(xx, y, zz).isAir()) {
										// 2: tell clients, 16: no neighbour updates, 32: no drops.
										level.setBlock(new net.minecraft.core.BlockPos((x << 4) + xx, baseY + y, (z << 4) + zz), air, 2 | 16 | 32);
										removed++;
									}
								}
							}
						}
					}
				}
			}
		}
		java.util.List<net.minecraft.world.entity.Entity> gone = new java.util.ArrayList<>();
		for (var entity : level.getAllEntities()) {
			if (!(entity instanceof net.minecraft.world.entity.player.Player)) {
				gone.add(entity);
			}
		}
		for (var entity : gone) {
			if (entity.isPassenger()) {
				entity.stopRiding();
			}
			entity.discard();
			removed++;
		}
		LOG.info("FNAFSBCraft: /clearworld removed {} blocks, entities and holes", removed);
		return removed;
	}

	/** The mirror world is a void that only exists to host the player; Skyrim drives time and spawning. */
	private static void configureServer(MinecraftServer server) {
		GameRules rules = server.getGameRules();
		rules.set(GameRules.ADVANCE_TIME, false, server);
		rules.set(GameRules.ADVANCE_WEATHER, false, server);
		rules.set(GameRules.SPAWN_MOBS, false, server);
		rules.set(GameRules.SPAWN_MONSTERS, false, server);
		rules.set(GameRules.SPAWN_PHANTOMS, false, server);
		rules.set(GameRules.SPAWN_PATROLS, false, server);
		rules.set(GameRules.SPAWN_WANDERING_TRADERS, false, server);
		rules.set(GameRules.PLAYER_MOVEMENT_CHECK, false, server);
		rules.set(GameRules.KEEP_INVENTORY, true, server);
		rules.set(GameRules.IMMEDIATE_RESPAWN, true, server);
		rules.set(GameRules.SHOW_ADVANCEMENT_MESSAGES, false, server);
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set noon");
		LOG.info("FNAFSBCraft: mirror world configured");
	}

	/**
	 * Local multiplayer test guests (tools/fake_guest.py; named Guest, Guest2, ...) wear a random
	 * mix of iron and diamond armour, so they're easy to tell apart.
	 */
	private static void dressTestGuest(ServerPlayer player) {
		if (!player.getName().getString().startsWith("Guest")) {
			return;
		}
		var random = player.getRandom();
		EquipmentSlot[] slots = { EquipmentSlot.HEAD, EquipmentSlot.CHEST, EquipmentSlot.LEGS, EquipmentSlot.FEET };
		net.minecraft.world.item.Item[][] pieces = {
			{ Items.IRON_HELMET, Items.DIAMOND_HELMET },
			{ Items.IRON_CHESTPLATE, Items.DIAMOND_CHESTPLATE },
			{ Items.IRON_LEGGINGS, Items.DIAMOND_LEGGINGS },
			{ Items.IRON_BOOTS, Items.DIAMOND_BOOTS },
		};
		for (int i = 0; i < slots.length; i++) {
			player.setItemSlot(slots[i], new ItemStack(pieces[i][random.nextBoolean() ? 1 : 0]));
		}
		LOG.info("FNAFSBCraft: dressed test guest {} in iron and diamond", player.getName().getString());
	}

	private static void giveStarterKit(ServerPlayer player) {
		if (!player.getInventory().isEmpty()) {
			return;
		}
		player.getInventory().add(new ItemStack(Items.DIAMOND_SWORD));
		player.getInventory().add(dev.skycraft.world.SkyDig.PASSTHROUGH ? pizzaplexPickaxe(player) : new ItemStack(Items.DIAMOND_PICKAXE));
		player.getInventory().add(new ItemStack(Items.BOW));
		player.getInventory().add(new ItemStack(Items.COOKED_BEEF, 32));
		player.getInventory().add(new ItemStack(Items.OAK_PLANKS, 64));
		player.getInventory().add(new ItemStack(Items.TORCH, 32));
		player.getInventory().add(new ItemStack(Items.ARROW, 64));
		player.setItemSlot(net.minecraft.world.entity.EquipmentSlot.OFFHAND, new ItemStack(Items.SHIELD));
		LOG.info("FNAFSBCraft: gave starter kit to {}", player.getName().getString());
	}

	/**
	 * Once per player: armor (Skyrim's enemies hit back now) and building materials, since there is
	 * no Minecraft terrain to mine in Skyrim.
	 */
	private static void giveBuilderKit(ServerPlayer player) {
		if (player.entityTags().contains(KIT2_TAG)) {
			return;
		}
		equipIfEmpty(player, EquipmentSlot.HEAD, Items.IRON_HELMET);
		equipIfEmpty(player, EquipmentSlot.CHEST, Items.IRON_CHESTPLATE);
		equipIfEmpty(player, EquipmentSlot.LEGS, Items.IRON_LEGGINGS);
		equipIfEmpty(player, EquipmentSlot.FEET, Items.IRON_BOOTS);
		var inventory = player.getInventory();
		inventory.add(new ItemStack(Items.COBBLESTONE, 64));
		inventory.add(new ItemStack(Items.STONE_BRICKS, 64));
		inventory.add(new ItemStack(Items.OAK_LOG, 64));
		inventory.add(new ItemStack(Items.GLASS, 64));
		inventory.add(new ItemStack(Items.OAK_STAIRS, 64));
		inventory.add(new ItemStack(Items.OAK_SLAB, 64));
		inventory.add(new ItemStack(Items.OAK_DOOR, 8));
		inventory.add(new ItemStack(Items.LADDER, 32));
		inventory.add(new ItemStack(Items.LANTERN, 16));
		inventory.add(new ItemStack(Items.CRAFTING_TABLE));
		inventory.add(new ItemStack(Items.WATER_BUCKET));
		inventory.add(new ItemStack(Items.ARROW, 64));
		inventory.add(new ItemStack(Items.GOLDEN_APPLE, 4));
		player.addTag(KIT2_TAG);
		LOG.info("FNAFSBCraft: gave builder kit to {}", player.getName().getString());
	}

	/**
	 * Once per player, Security Breach only: what the other kits lack for living off nothing in the
	 * Pizzaplex. Water and lava make a cobblestone generator; two water buckets make endless water;
	 * saplings need the dirt.
	 */
	private static void givePizzaplexKit(ServerPlayer player) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || player.entityTags().contains(KIT3_TAG)) {
			return;
		}
		var inventory = player.getInventory();
		// The starter kit only goes to an empty inventory: make sure of the pickaxe.
		if (!inventory.contains(new ItemStack(Items.DIAMOND_PICKAXE)) && !inventory.contains(new ItemStack(Items.IRON_PICKAXE))) {
			inventory.add(pizzaplexPickaxe(player));
		}
		inventory.add(new ItemStack(Items.IRON_HOE));
		inventory.add(new ItemStack(Items.LAVA_BUCKET));
		inventory.add(new ItemStack(Items.WATER_BUCKET));
		inventory.add(new ItemStack(Items.OAK_TRAPDOOR, 16));
		inventory.add(new ItemStack(Items.OAK_SAPLING, 8));
		inventory.add(new ItemStack(Items.DIRT, 32));
		inventory.add(new ItemStack(Items.BONE_MEAL, 16));
		inventory.add(new ItemStack(VOID_FILLER));
		player.addTag(KIT3_TAG);
		LOG.info("FNAFSBCraft: gave Pizzaplex kit to {}", player.getName().getString());
	}

	/** Security Breach's pickaxe: iron with Unbreaking III (diamond mined the building too fast). */
	private static ItemStack pizzaplexPickaxe(ServerPlayer player) {
		var pickaxe = new ItemStack(Items.IRON_PICKAXE);
		var unbreaking = player.level().registryAccess().lookupOrThrow(net.minecraft.core.registries.Registries.ENCHANTMENT)
			.get(net.minecraft.world.item.enchantment.Enchantments.UNBREAKING);
		unbreaking.ifPresent(holder -> pickaxe.enchant(holder, 3));
		return pickaxe;
	}

	/**
	 * Once per player, Security Breach only: the diamond pickaxe of the starter kit becomes the iron
	 * one. And barriers an earlier version put beside holes (they couldn't be mined) go.
	 */
	private static void fixPizzaplexKit(ServerPlayer player) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH) {
			return;
		}
		if (!player.entityTags().contains(KIT4_TAG)) {
			var inventory = player.getInventory();
			for (int slot = 0; slot < inventory.getContainerSize(); slot++) {
				var stack = inventory.getItem(slot);
				if (stack.is(Items.DIAMOND_PICKAXE) && !stack.isEnchanted()) {
					inventory.setItem(slot, pizzaplexPickaxe(player));
					LOG.info("FNAFSBCraft: {}'s diamond pickaxe is an iron one now", player.getName().getString());
				}
			}
			player.addTag(KIT4_TAG);
		}
		var level = player.level();
		var centre = player.blockPosition();
		int removed = 0;
		for (var pos : net.minecraft.core.BlockPos.betweenClosed(centre.offset(-32, -16, -32), centre.offset(32, 16, 32))) {
			if (level.getBlockState(pos).is(net.minecraft.world.level.block.Blocks.BARRIER)) {
				level.setBlock(pos, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 3);
				removed++;
			}
		}
		if (removed > 0) {
			LOG.info("FNAFSBCraft: {} barriers from an earlier version removed", removed);
		}
	}

	private static void equipIfEmpty(ServerPlayer player, EquipmentSlot slot, net.minecraft.world.item.Item item) {
		if (player.getItemBySlot(slot).isEmpty()) {
			player.setItemSlot(slot, new ItemStack(item));
		} else {
			player.getInventory().add(new ItemStack(item));
		}
	}
}
