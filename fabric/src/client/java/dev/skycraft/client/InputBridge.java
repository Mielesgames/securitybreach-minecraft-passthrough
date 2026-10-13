package dev.skycraft.client;

import dev.skycraft.combat.SkyCombat;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonInfo;
import org.lwjgl.sdl.SDLKeyboard;

/**
 * Replays Skyrim-captured input into Minecraft's own input handlers, as if the (hidden) MC
 * window had focus. Keeps a virtual keyboard so InputConstants.isKeyDown() still works.
 */
public final class InputBridge {
	private static final boolean[] KEYS = new boolean[512];
	private static final boolean[] BUTTONS = new boolean[8];
	private static double cursorX, cursorY;
	private static int modifiers;
	private static int clickLogs;

	private InputBridge() {
	}

	public static boolean isKeyDown(int scancode) {
		return scancode >= 0 && scancode < KEYS.length && KEYS[scancode];
	}

	public static void drain(Minecraft minecraft) {
		SkyLink.drainInput((type, code, a, b, c) -> dispatch(minecraft, type, code, a, b, c));
	}

	private static void dispatch(Minecraft minecraft, int type, int code, int a, int b, int c) {
		long handle = minecraft.getWindow().handle();
		switch (type) {
			case Proto.IN_KEY -> key(minecraft, handle, code, a != 0);
			case Proto.IN_MOUSE_BUTTON -> {
				if (code > 0 && code < BUTTONS.length) {
					BUTTONS[code] = a != 0;
				}
				if (a != 0 && clickLogs++ < 20) {
					var hit = minecraft.hitResult;
					dev.skycraft.SkyCraft.LOG.info("SkyCraft: click {} -> {} {} (grabbed {}, screen {})", code, hit == null ? "null" : hit.getType(),
						hit instanceof net.minecraft.world.phys.EntityHitResult eh ? eh.getEntity().getName().getString() : hit == null ? "" : hit.getLocation(),
						minecraft.mouseHandler.isMouseGrabbed(), minecraft.gui.screen());
				}
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(code, modifiers), a != 0 ? 1 : 0);
			}
			case Proto.IN_SCROLL -> minecraft.mouseHandler.onScroll(handle, 0.0, a / 120.0);
			case Proto.IN_CURSOR -> {
				double dx = a - cursorX;
				double dy = b - cursorY;
				cursorX = a;
				cursorY = b;
				minecraft.mouseHandler.onMove(handle, a, b, dx, dy);
			}
			case Proto.IN_TEXT -> {
				if (minecraft.gui.screen() != null) {
					minecraft.keyboardHandler.textInput(handle, new String(Character.toChars(a)));
				}
			}
			case Proto.IN_RELEASE_ALL -> releaseAll();
			case Proto.IN_HURT -> hurt(minecraft, code, a / 100.0F, b, c);
			case Proto.IN_OPEN_MENU -> {
				if (minecraft.gui.screen() == null && minecraft.player != null) {
					releaseAll();
					minecraft.gui.setScreen(new PauseScreen(true));
				}
			}
			case Proto.IN_SWING -> {
				if (minecraft.player != null) {
					minecraft.player.swing(net.minecraft.world.InteractionHand.MAIN_HAND, minecraft.player.getMainHandItem().getAttackAnimation(), false);
				}
			}
			case Proto.IN_BREAK_EFFECT -> {
				// Something of the host game's broke (a cardboard cutout): it goes like a block of planks.
				if (minecraft.level != null) {
					var pos = net.minecraft.core.BlockPos.containing(a / 100.0, b / 100.0, c / 100.0);
					var planks = net.minecraft.world.level.block.Blocks.OAK_PLANKS.defaultBlockState();
					minecraft.level.addDestroyBlockEffect(pos, planks);
					minecraft.level.addDestroyBlockEffect(pos.above(), planks);
					minecraft.level.playLocalSound(pos, net.minecraft.sounds.SoundEvents.WOOD_BREAK, net.minecraft.sounds.SoundSource.BLOCKS, 1.0F, 0.8F, false);
				}
			}
			case Proto.IN_BUCKET_TOKEN -> {
				// A Security Breach character was scooped up: the water bucket in hand now holds it.
				var name = TOKEN_NAMES.remove(code);
				var server = minecraft.getSingleplayerServer();
				if (server != null && minecraft.player != null) {
					String label = name == null || name.isEmpty() ? "Animatronic" : name.toString();
					var uuid = minecraft.player.getUUID();
					server.execute(() -> {
						var player = server.getPlayerList().getPlayer(uuid);
						if (player == null) {
							return;
						}
						var bucket = dev.skycraft.SkyCraft.tokenBucket(code, label);
						if (player.getMainHandItem().is(net.minecraft.world.item.Items.WATER_BUCKET) && dev.skycraft.SkyCraft.tokenOf(player.getMainHandItem()) == 0) {
							player.setItemInHand(net.minecraft.world.InteractionHand.MAIN_HAND, bucket);
						} else {
							player.getInventory().add(bucket);
						}
					});
				}
			}
			case Proto.IN_LEASH -> {
				// The player used a lead on a Security Breach character, or took it off.
				var server = minecraft.getSingleplayerServer();
				if (server != null && minecraft.player != null) {
					var uuid = minecraft.player.getUUID();
					boolean on = code != 0;
					int formId = a;
					server.execute(() -> {
						var player = server.getPlayerList().getPlayer(uuid);
						if (player != null) {
							dev.skycraft.combat.SkyCombat.leash(player, formId, on);
						}
					});
				}
			}
			case Proto.IN_PICK_TOKEN -> {
				// Creative pick on a Security Breach thing: its spawn egg or paper, into the hand.
				var name = TOKEN_NAMES.remove(code);
				var server = minecraft.getSingleplayerServer();
				if (server != null && minecraft.player != null) {
					String label = name == null || name.isEmpty() ? "Object" : name.toString();
					boolean character = a != 0;
					var uuid = minecraft.player.getUUID();
					server.execute(() -> {
						var player = server.getPlayerList().getPlayer(uuid);
						if (player == null) {
							return;
						}
						var item = character ? dev.skycraft.SkyCraft.tokenEgg(code, label) : dev.skycraft.SkyCraft.tokenPaper(code, label);
						if (player.getMainHandItem().isEmpty()) {
							player.setItemInHand(net.minecraft.world.InteractionHand.MAIN_HAND, item);
						} else {
							player.getInventory().add(item);
						}
					});
				}
			}
			case Proto.IN_CHAT_TEXT -> {
				var text = CHAT_LINES.computeIfAbsent(code, k -> new StringBuilder());
				for (int word : new int[] { a, b, c }) {
					for (int i = 0; i < 4; i++) {
						int ch = (word >>> (i * 8)) & 0xFF;
						if (ch != 0) {
							text.append((char) ch);
						}
					}
				}
			}
			case Proto.IN_CHAT_SAY -> {
				// Something a Security Breach character said (its subtitle): into the chat.
				var text = CHAT_LINES.remove(code);
				if (text != null && !text.isEmpty()) {
					minecraft.gui.hud.getChat().addClientSystemMessage(net.minecraft.network.chat.Component.literal(text.toString()));
				}
			}
			case Proto.IN_HIDE -> {
				// The host player is in a hiding spot (or left it): onto the seat, or the seat goes.
				var server = minecraft.getSingleplayerServer();
				if (server != null && minecraft.player != null) {
					var uuid = minecraft.player.getUUID();
					boolean hide = code != 0;
					double x = a / 100.0, y = b / 100.0, z = c / 100.0;
					server.execute(() -> {
						var player = server.getPlayerList().getPlayer(uuid);
						if (player != null) {
							dev.skycraft.combat.HidingSeat.set(player, hide, x, y, z);
						}
					});
				}
			}
			case Proto.IN_MOUNT_FREDDY -> {
				// The player used Freddy in the host game: onto his mule.
				var server = minecraft.getSingleplayerServer();
				if (server != null && minecraft.player != null) {
					var uuid = minecraft.player.getUUID();
					double x = a / 100.0, y = b / 100.0, z = c / 100.0;
					server.execute(() -> {
						var player = server.getPlayerList().getPlayer(uuid);
						if (player != null) {
							dev.skycraft.combat.FreddyMount.mount(player, x, y, z);
						}
					});
				}
			}
			case Proto.IN_TOKEN_NAME -> {
				var name = TOKEN_NAMES.computeIfAbsent(code, k -> new StringBuilder());
				for (int word : new int[] { a, b, c }) {
					for (int i = 0; i < 4; i++) {
						int ch = (word >>> (i * 8)) & 0xFF;
						if (ch != 0) {
							name.append((char) ch);
						}
					}
				}
			}
			case Proto.IN_DROP_TOKEN -> {
				// A Security Breach object broke: its paper drops where it stood, like a block's drop.
				var name = TOKEN_NAMES.remove(code);
				var server = minecraft.getSingleplayerServer();
				if (server != null && minecraft.player != null) {
					String label = name == null || name.isEmpty() ? "Object" : name.toString();
					double x = a / 100.0, y = b / 100.0, z = c / 100.0;
					var dimension = minecraft.player.level().dimension();
					server.execute(() -> {
						var level = server.getLevel(dimension);
						if (level != null) {
							var drop = new net.minecraft.world.entity.item.ItemEntity(level, x, y + 0.5, z, dev.skycraft.SkyCraft.tokenPaper(code, label));
							drop.setDefaultPickUpDelay();
							level.addFreshEntity(drop);
						}
					});
				}
			}
			case Proto.IN_USE_TOKEN -> {
				// The object was put back: the paper in the hand is used up (not in creative).
				var server = minecraft.getSingleplayerServer();
				if (server != null && minecraft.player != null) {
					var uuid = minecraft.player.getUUID();
					server.execute(() -> {
						var player = server.getPlayerList().getPlayer(uuid);
						if (player != null && !player.getAbilities().instabuild && dev.skycraft.SkyCraft.tokenOf(player.getMainHandItem()) == code) {
							if (player.getMainHandItem().is(net.minecraft.world.item.Items.WATER_BUCKET)) {
								// The character is out; the water stays in the bucket.
								player.setItemInHand(net.minecraft.world.InteractionHand.MAIN_HAND, new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.WATER_BUCKET));
							} else {
								player.getMainHandItem().shrink(1);
							}
						}
					});
				}
			}
			default -> {
			}
		}
	}

	private static final java.util.Map<Integer, StringBuilder> TOKEN_NAMES = new java.util.HashMap<>();
	private static final java.util.Map<Integer, StringBuilder> CHAT_LINES = new java.util.HashMap<>();

	/** Skyrim hit the player: apply it as Minecraft damage on the integrated server (or the host's). */
	private static void hurt(Minecraft minecraft, int kind, float skyrimDamage, int attacker, int flags) {
		var server = minecraft.getSingleplayerServer();
		if (minecraft.player == null) {
			return;
		}
		if (server == null) {
			// A guest in a friend's world: the host's server applies it.
			if (net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.canSend(dev.skycraft.net.SkyNet.Hurt.TYPE)) {
				net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.send(new dev.skycraft.net.SkyNet.Hurt(kind, skyrimDamage, attacker, flags));
			}
			return;
		}
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null) {
				SkyCombat.hurtPlayer(player, kind, skyrimDamage, attacker, flags);
			}
		});
	}

	private static void key(Minecraft minecraft, long handle, int scancode, boolean down) {
		if (scancode <= 0 || scancode >= KEYS.length) {
			return;
		}
		boolean wasDown = KEYS[scancode];
		KEYS[scancode] = down;
		updateModifiers();
		int action = down ? (wasDown ? -1 : 1) : 0; // -1 = repeat
		int keycode = SDLKeyboard.SDL_GetKeyFromScancode(scancode, (short) modifiers, true);
		minecraft.keyboardHandler.keyPress(handle, action, new KeyEvent(scancode, keycode, modifiers));
	}

	private static void updateModifiers() {
		int m = 0;
		if (KEYS[225]) m |= 0x0001; // SDL_KMOD_LSHIFT
		if (KEYS[229]) m |= 0x0002; // SDL_KMOD_RSHIFT
		if (KEYS[224]) m |= 0x0040; // SDL_KMOD_LCTRL
		if (KEYS[228]) m |= 0x0080; // SDL_KMOD_RCTRL
		if (KEYS[226]) m |= 0x0100; // SDL_KMOD_LALT
		if (KEYS[230]) m |= 0x0200; // SDL_KMOD_RALT
		modifiers = m;
	}

	/** Lift every key and button we think is held (focus moved to Skyrim, link dropped, ...). */
	public static void releaseAll() {
		Minecraft minecraft = Minecraft.getInstance();
		long handle = minecraft.getWindow().handle();
		for (int sc = 0; sc < KEYS.length; sc++) {
			if (KEYS[sc]) {
				KEYS[sc] = false;
				updateModifiers();
				minecraft.keyboardHandler.keyPress(handle, 0, new KeyEvent(sc, SDLKeyboard.SDL_GetKeyFromScancode(sc, (short) 0, true), modifiers));
			}
		}
		for (int button = 1; button < BUTTONS.length; button++) {
			if (BUTTONS[button]) {
				BUTTONS[button] = false;
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(button, 0), 0);
			}
		}
	}
}
