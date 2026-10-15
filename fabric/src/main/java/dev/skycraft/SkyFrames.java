package dev.skycraft;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import net.fabricmc.fabric.api.attachment.v1.AttachmentRegistry;
import net.fabricmc.fabric.api.attachment.v1.AttachmentType;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.event.player.AttackBlockCallback;
import net.fabricmc.fabric.api.event.player.PlayerBlockBreakEvents;
import net.fabricmc.fabric.api.event.player.UseBlockCallback;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.ChatFormatting;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.component.DataComponents;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.component.CustomData;
import net.minecraft.world.item.context.BlockPlaceContext;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.EndPortalFrameBlock;

/**
 * Security Breach: nine end portal frames hidden in the Pizzaplex. Each lies at a marker (a
 * position in Minecraft's coordinates, the same for every world, since the map is the game's).
 * Clicking one picks it up ("3 of 9 end portal frames found"); frames can't be dropped or placed
 * one by one, and with all nine a click builds the whole portal at once.
 *
 * The markers are set in creative (so with allow_cheating on): reinforced deepslate put down sets
 * one and stays there to show it, breaking it in creative takes it away. Frames themselves go
 * down normally in creative. They are kept in fnafsbcraft_end_frames.txt in
 * Minecraft's folder, which takes over from the list that comes with the mod
 * (data/skycraft/end_frames.txt).
 */
public final class SkyFrames {
	public static final int REQUIRED = 9;
	private static final String FRAME_KEY = "skycraft_end_frame";
	private static final String BUNDLED = "/data/skycraft/end_frames.txt";
	private static final double REACH = 64.0;
	/** Put down in creative, it sets a marker (and stays, to show where). */
	public static final net.minecraft.world.item.Item MARKER = Items.REINFORCED_DEEPSLATE;

	/** The markers this player has picked up (packed block positions). */
	public static final AttachmentType<List<Long>> FOUND = AttachmentRegistry.<List<Long>>builder()
		.persistent(com.mojang.serialization.Codec.LONG.listOf())
		.copyOnDeath()
		.buildAndRegister(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "end_frames_found"));

	private static Set<Long> markers;
	private static int ticks;

	private SkyFrames() {
	}

	public static void init() {
		SkyCraft.LOG.info("FNAFSBCraft: {} end portal frame markers ({}), found frames kept as {}", markers().size(), localFile(), FOUND.identifier());
		UseBlockCallback.EVENT.register((player, level, hand, hit) -> {
			if (!dev.skycraft.world.SkyDig.PASSTHROUGH) {
				return InteractionResult.PASS;
			}
			var pos = hit.getBlockPos();
			// A hidden frame: picked up (not in creative, where it is a marker to edit).
			if (!player.getAbilities().instabuild && level.getBlockState(pos).is(Blocks.END_PORTAL_FRAME) && markers().contains(pos.asLong())) {
				if (player instanceof ServerPlayer server) {
					pickUp(server, pos);
				}
				return InteractionResult.SUCCESS;
			}
			var stack = player.getItemInHand(hand);
			if (player.getAbilities().instabuild && stack.is(MARKER)) {
				// Creative: the marker block goes down as itself (it shows where it is), and the
				// spot is written down. Where it stands no frame is laid (it isn't air).
				var place = new BlockPlaceContext(player, hand, stack, hit);
				if (!level.isClientSide() && level.getBlockState(place.getClickedPos()).canBeReplaced()) {
					edit(place.getClickedPos(), true);
				}
				return InteractionResult.PASS;
			}
			// In creative frames go down like any block.
			if (!stack.is(Items.END_PORTAL_FRAME) || player.getAbilities().instabuild) {
				return InteractionResult.PASS;
			}
			var place = new BlockPlaceContext(player, hand, stack, hit);
			if (!level.isClientSide()) {
				int have = countFrames(player);
				if (have < REQUIRED) {
					player.sendSystemMessage(Component.literal("You need all " + REQUIRED + " end portal frames to build the portal (" + have + " of " + REQUIRED + ")")
						.withStyle(ChatFormatting.YELLOW));
				} else if (build((ServerLevel) level, place.getClickedPos(), player.getDirection())) {
					takeFrames(player, REQUIRED);
					SkyCraft.LOG.info("FNAFSBCraft: {} built the end portal at {}", player.getName().getString(), place.getClickedPos());
				} else {
					player.sendSystemMessage(Component.literal("Not enough room for the end portal here").withStyle(ChatFormatting.YELLOW));
				}
			}
			return InteractionResult.SUCCESS;
		});
		// Punching a hidden frame picks it up too (it can't be mined).
		AttackBlockCallback.EVENT.register((player, level, hand, pos, direction) -> {
			if (!dev.skycraft.world.SkyDig.PASSTHROUGH || player.getAbilities().instabuild
				|| !level.getBlockState(pos).is(Blocks.END_PORTAL_FRAME) || !markers().contains(pos.asLong())) {
				return InteractionResult.PASS;
			}
			if (player instanceof ServerPlayer server) {
				pickUp(server, pos);
			}
			return InteractionResult.SUCCESS;
		});
		// Creative: the marker block (or a frame) broken where a marker was takes the marker away.
		PlayerBlockBreakEvents.AFTER.register((level, player, pos, state, blockEntity) -> {
			if (dev.skycraft.world.SkyDig.PASSTHROUGH && (state.is(Blocks.END_PORTAL_FRAME) || state.is(Blocks.REINFORCED_DEEPSLATE)) && player.getAbilities().instabuild && !level.isClientSide()) {
				edit(pos, false);
			}
		});
		ServerTickEvents.END_SERVER_TICK.register(SkyFrames::tick);
	}

	/** The markers: Minecraft's own list once one was set, otherwise the mod's. */
	private static Set<Long> markers() {
		if (markers == null) {
			markers = new LinkedHashSet<>();
			try {
				Path local = localFile();
				if (Files.exists(local)) {
					read(Files.newInputStream(local));
				} else {
					InputStream bundled = SkyFrames.class.getResourceAsStream(BUNDLED);
					if (bundled != null) {
						read(bundled);
					}
				}
			} catch (Exception e) {
				SkyCraft.LOG.warn("FNAFSBCraft: couldn't read the end portal frame markers", e);
			}
		}
		return markers;
	}

	private static Path localFile() {
		return FabricLoader.getInstance().getGameDir().resolve("fnafsbcraft_end_frames.txt");
	}

	/** Lines "x y z"; anything after # is a note. */
	private static void read(InputStream in) throws Exception {
		try (var reader = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8))) {
			String line;
			while ((line = reader.readLine()) != null) {
				int hash = line.indexOf('#');
				String[] parts = (hash >= 0 ? line.substring(0, hash) : line).trim().split("\\s+");
				if (parts.length == 3) {
					markers.add(BlockPos.asLong(Integer.parseInt(parts[0]), Integer.parseInt(parts[1]), Integer.parseInt(parts[2])));
				}
			}
		}
	}

	/** A marker set or taken away in creative; the whole list is written back. */
	private static void edit(BlockPos pos, boolean add) {
		boolean changed = add ? markers().add(pos.asLong()) : markers().remove(pos.asLong());
		if (!changed) {
			return;
		}
		var lines = new ArrayList<String>();
		lines.add("# FNAFSBCraft: where the end portal frames are hidden (Minecraft x y z). Set in creative.");
		for (long marker : markers) {
			var at = BlockPos.of(marker);
			lines.add(at.getX() + " " + at.getY() + " " + at.getZ());
		}
		try {
			Files.write(localFile(), lines, StandardCharsets.UTF_8);
		} catch (Exception e) {
			SkyCraft.LOG.warn("FNAFSBCraft: couldn't save the end portal frame markers", e);
		}
		SkyCraft.LOG.info("FNAFSBCraft: end portal frame marker {} at {} ({} markers now)", add ? "set" : "removed", pos, markers.size());
	}

	private static boolean isFrame(ItemStack stack) {
		if (!stack.is(Items.END_PORTAL_FRAME)) {
			return false;
		}
		var data = stack.get(DataComponents.CUSTOM_DATA);
		return data != null && data.copyTag().getBooleanOr(FRAME_KEY, false);
	}

	private static ItemStack frameItem() {
		var stack = new ItemStack(Items.END_PORTAL_FRAME);
		var tag = new CompoundTag();
		tag.putBoolean(FRAME_KEY, true);
		stack.set(DataComponents.CUSTOM_DATA, CustomData.of(tag));
		return stack;
	}

	private static int countFrames(Player player) {
		int count = 0;
		var inventory = player.getInventory();
		for (int i = 0; i < inventory.getContainerSize(); i++) {
			if (isFrame(inventory.getItem(i))) {
				count += inventory.getItem(i).getCount();
			}
		}
		return count;
	}

	private static void takeFrames(Player player, int amount) {
		var inventory = player.getInventory();
		for (int i = 0; i < inventory.getContainerSize() && amount > 0; i++) {
			var stack = inventory.getItem(i);
			if (isFrame(stack)) {
				int take = Math.min(amount, stack.getCount());
				stack.shrink(take);
				amount -= take;
			}
		}
	}

	private static void pickUp(ServerPlayer player, BlockPos pos) {
		long key = pos.asLong();
		var found = new ArrayList<>(player.getAttachedOrElse(FOUND, List.of()));
		if (!found.contains(key)) {
			found.add(key);
			player.setAttached(FOUND, List.copyOf(found));
		}
		player.level().setBlock(pos, Blocks.AIR.defaultBlockState(), 3);
		player.getInventory().add(frameItem());
		// More are hidden than are needed: past nine they are spares.
		player.sendSystemMessage(Component.literal(found.size() <= REQUIRED
			? found.size() + " of " + REQUIRED + " end portal frames found"
			: "A spare end portal frame found (" + found.size() + " of " + markers().size() + ")").withStyle(ChatFormatting.YELLOW));
		if (found.size() == REQUIRED) {
			player.sendSystemMessage(Component.literal("All " + REQUIRED + " found. Put them down to build the end portal.").withStyle(ChatFormatting.YELLOW));
		}
		player.level().playSound(null, pos, net.minecraft.sounds.SoundEvents.END_PORTAL_FRAME_FILL, net.minecraft.sounds.SoundSource.BLOCKS, 1.0F, 1.0F);
		SkyCraft.LOG.info("FNAFSBCraft: {} found end portal frame {} of {} at {}", player.getName().getString(), found.size(), REQUIRED, pos);
	}

	/**
	 * The whole portal at once: twelve frames with their eyes around a 3x3 portal. The clicked
	 * spot is the nearest row of frames; the portal lies beyond it, the way the player looks.
	 */
	private static boolean build(ServerLevel level, BlockPos near, Direction facing) {
		if (facing.getAxis() == Direction.Axis.Y) {
			facing = Direction.NORTH;
		}
		var center = near.relative(facing, 2);
		var frames = new ArrayList<BlockPos>();
		var directions = new ArrayList<Direction>();
		for (int i = -1; i <= 1; i++) {
			frames.add(center.offset(i, 0, -2));
			directions.add(Direction.SOUTH);
			frames.add(center.offset(i, 0, 2));
			directions.add(Direction.NORTH);
			frames.add(center.offset(-2, 0, i));
			directions.add(Direction.EAST);
			frames.add(center.offset(2, 0, i));
			directions.add(Direction.WEST);
		}
		var inside = new ArrayList<BlockPos>();
		for (int dx = -1; dx <= 1; dx++) {
			for (int dz = -1; dz <= 1; dz++) {
				inside.add(center.offset(dx, 0, dz));
			}
		}
		for (var cell : frames) {
			if (!level.getBlockState(cell).canBeReplaced()) {
				return false;
			}
		}
		for (var cell : inside) {
			if (!level.getBlockState(cell).canBeReplaced()) {
				return false;
			}
		}
		for (int i = 0; i < frames.size(); i++) {
			level.setBlock(frames.get(i), Blocks.END_PORTAL_FRAME.defaultBlockState()
				.setValue(EndPortalFrameBlock.FACING, directions.get(i)).setValue(EndPortalFrameBlock.HAS_EYE, true), 3);
		}
		for (var cell : inside) {
			level.setBlock(cell, Blocks.END_PORTAL.defaultBlockState(), 3);
		}
		level.playSound(null, center, net.minecraft.sounds.SoundEvents.END_PORTAL_SPAWN, net.minecraft.sounds.SoundSource.BLOCKS, 1.0F, 1.0F);
		return true;
	}

	private static void tick(MinecraftServer server) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || ++ticks % 10 != 0) {
			return;
		}
		ServerLevel level = server.overworld();
		// Frames can't be dropped: one thrown out goes back to whoever threw it.
		for (var item : level.getEntities(net.minecraft.world.entity.EntityTypes.ITEM, (net.minecraft.world.entity.item.ItemEntity entity) -> isFrame(entity.getItem()))) {
			Player back = item.getOwner() instanceof Player owner ? owner : level.getNearestPlayer(item, 32.0);
			if (back != null && back.getInventory().add(item.getItem().copy())) {
				item.discard();
			}
		}
		if (ticks % 40 != 0 || markers().isEmpty()) {
			return;
		}
		// The hidden frames lie at their markers, for whoever hasn't found them yet.
		for (ServerPlayer player : level.players()) {
			var found = player.getAttachedOrElse(FOUND, List.<Long>of());
			for (long marker : markers) {
				var pos = BlockPos.of(marker);
				if (found.contains(marker) || !pos.closerToCenterThan(player.position(), REACH) || !level.isLoaded(pos)) {
					continue;
				}
				var state = level.getBlockState(pos);
				if (!state.is(Blocks.END_PORTAL_FRAME) && state.canBeReplaced()) {
					level.setBlock(pos, Blocks.END_PORTAL_FRAME.defaultBlockState(), 3);
				}
			}
		}
	}
}
