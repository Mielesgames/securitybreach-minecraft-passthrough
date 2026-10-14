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

	/** Security Breach: a water bucket scoops up an animatronic, like a fish. */
	public static final net.minecraft.world.level.gamerules.GameRule<Boolean> ALLOW_ANIMATRONIC_WATER_BUCKET =
		net.fabricmc.fabric.api.gamerule.v1.GameRuleBuilder.forBoolean(true)
			.category(net.minecraft.world.level.gamerules.GameRuleCategory.PLAYER)
			.buildAndRegister(net.minecraft.resources.Identifier.fromNamespaceAndPath("skycraft", "allow_animatronic_water_bucket"));

	/** The bucket a scooped-up Security Breach character is in: its name, and the host's number for it. */
	public static net.minecraft.world.item.ItemStack tokenBucket(int id, String name) {
		var bucket = new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.WATER_BUCKET);
		bucket.set(net.minecraft.core.component.DataComponents.CUSTOM_NAME, net.minecraft.network.chat.Component.literal("Bucket of " + name));
		bucket.set(net.minecraft.core.component.DataComponents.ENCHANTMENT_GLINT_OVERRIDE, true);
		var tag = new net.minecraft.nbt.CompoundTag();
		tag.putInt(TOKEN_KEY, id);
		bucket.set(net.minecraft.core.component.DataComponents.CUSTOM_DATA, net.minecraft.world.item.component.CustomData.of(tag));
		return bucket;
	}

	/** The spawn egg of a Security Breach character (creative pick): its name, and the host's number for it. */
	public static net.minecraft.world.item.ItemStack tokenEgg(int id, String name) {
		var egg = new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.POLAR_BEAR_SPAWN_EGG);
		egg.set(net.minecraft.core.component.DataComponents.CUSTOM_NAME, net.minecraft.network.chat.Component.literal(name + " Spawn Egg"));
		var tag = new net.minecraft.nbt.CompoundTag();
		tag.putInt(TOKEN_KEY, id);
		egg.set(net.minecraft.core.component.DataComponents.CUSTOM_DATA, net.minecraft.world.item.component.CustomData.of(tag));
		return egg;
	}

	/** The paper a broken Security Breach object leaves: its name, and the host's number for it. */
	public static net.minecraft.world.item.ItemStack tokenPaper(int id, String name) {
		var paper = new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.PAPER);
		paper.set(net.minecraft.core.component.DataComponents.CUSTOM_NAME, net.minecraft.network.chat.Component.literal(name));
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
		net.fabricmc.fabric.api.creativetab.v1.CreativeModeTabEvents.modifyOutputEvent(net.minecraft.world.item.CreativeModeTabs.FUNCTIONAL_BLOCKS)
			.register(output -> output.accept(INVERTED_ITEM));
		LOG.info("SkyCraft: inverted block registered as {}", INVERTED_BLOCK_KEY.identifier());
		dev.skycraft.net.SkyNet.init();
		dev.skycraft.world.SkyDig.init();
		// Commands for the host game (Security Breach): "/freddy" or "/tp freddy" brings that
		// character to the player, "/summon freddy" makes a new one. One per character. They are
		// extra branches of Minecraft's own /tp and /summon, so the chat completes them.
		net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
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
					LOG.info("SkyCraft: {} joined riding {}, taken off it", joined.getName().getString(), vehicle.getName().getString());
				}
			});
			giveStarterKit(handler.getPlayer());
			giveBuilderKit(handler.getPlayer());
			givePizzaplexKit(handler.getPlayer());
			dressTestGuest(handler.getPlayer());
		});
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
		LOG.info("SkyCraft: mirror world configured");
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
		LOG.info("SkyCraft: dressed test guest {} in iron and diamond", player.getName().getString());
	}

	private static void giveStarterKit(ServerPlayer player) {
		if (!player.getInventory().isEmpty()) {
			return;
		}
		player.getInventory().add(new ItemStack(Items.DIAMOND_SWORD));
		player.getInventory().add(new ItemStack(Items.DIAMOND_PICKAXE));
		player.getInventory().add(new ItemStack(Items.BOW));
		player.getInventory().add(new ItemStack(Items.COOKED_BEEF, 32));
		player.getInventory().add(new ItemStack(Items.OAK_PLANKS, 64));
		player.getInventory().add(new ItemStack(Items.TORCH, 32));
		player.getInventory().add(new ItemStack(Items.ARROW, 64));
		player.setItemSlot(net.minecraft.world.entity.EquipmentSlot.OFFHAND, new ItemStack(Items.SHIELD));
		LOG.info("SkyCraft: gave starter kit to {}", player.getName().getString());
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
		LOG.info("SkyCraft: gave builder kit to {}", player.getName().getString());
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
		if (!inventory.contains(new ItemStack(Items.DIAMOND_PICKAXE))) {
			inventory.add(new ItemStack(Items.DIAMOND_PICKAXE));
		}
		inventory.add(new ItemStack(Items.LAVA_BUCKET));
		inventory.add(new ItemStack(Items.WATER_BUCKET));
		inventory.add(new ItemStack(Items.OAK_TRAPDOOR, 16));
		inventory.add(new ItemStack(Items.OAK_SAPLING, 8));
		inventory.add(new ItemStack(Items.DIRT, 32));
		inventory.add(new ItemStack(Items.BONE_MEAL, 16));
		inventory.add(new ItemStack(INVERTED_ITEM, 16));
		player.addTag(KIT3_TAG);
		LOG.info("SkyCraft: gave Pizzaplex kit to {}", player.getName().getString());
	}

	private static void equipIfEmpty(ServerPlayer player, EquipmentSlot slot, net.minecraft.world.item.Item item) {
		if (player.getItemBySlot(slot).isEmpty()) {
			player.setItemSlot(slot, new ItemStack(item));
		} else {
			player.getInventory().add(new ItemStack(item));
		}
	}
}
