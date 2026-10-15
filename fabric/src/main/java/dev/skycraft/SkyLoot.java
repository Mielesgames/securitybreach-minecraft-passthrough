package dev.skycraft;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.component.DataComponents;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.network.Filterable;
import net.minecraft.util.RandomSource;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.component.WrittenBookContent;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.ChestBlock;
import net.minecraft.world.level.block.entity.ChestBlockEntity;

/**
 * Security Breach: the first time the player comes into one of the Pizzaplex's rooms there may be
 * a chest on its floor (the host picks the room and the place). In it: things for getting by, a
 * few for fun, and sometimes a book that tells one of the mod's hidden features.
 */
public final class SkyLoot {
	private SkyLoot() {
	}

	/** Hidden features, one a book: title, then what it says. */
	private static final String[][] SECRETS = {
		{ "Pumpkin Season", "Give an animatronic a carved pumpkin (right-click, or drop it at its feet).\n\nIt can't see a thing and stumbles around like a headless chicken.\n\nShears take it off again." },
		{ "Slippery Floors", "Ice, packed ice, blue ice or frost walker boots on an animatronic make the floor slippery for it.\n\nIt slides on in turns, and you can shove it away." },
		{ "Wings", "Drop an elytra at an animatronic's feet.\n\nMost of them hunt you from the air like phantoms. Freddy floats along like an allay." },
		{ "Hold Still", "A lead works on animatronics too. Tie them to a fence." },
		{ "A Fishy Business", "A water bucket scoops up an animatronic. Put it back wherever you like." },
		{ "Jockeys", "Some animatronics ride chickens. If you want one: /chica jockey." },
		{ "Upside Down", "A name tag called Dinnerbone or Grumm turns an animatronic upside down." },
		{ "Saddle Up", "Right-click Freddy to ride him. Press E while riding for his saddle and chest." },
		{ "Shield", "Raise a shield: animatronics in front of you bounce off, and lasers stop at it." },
		{ "Spooky Disguise", "Wearing a carved pumpkin, the animatronics see you a third as far.\n\nSneaking helps too, and it's silent." },
		{ "Iron Guard", "Iron golems hate animatronics. Build one and watch." },
		{ "Experiments", "/experiment enable block_breaking lets a pickaxe dig into the Pizzaplex itself.\n\nA Void Filler closes the holes again." },
	};

	public static void place(ServerLevel level, double x, double y, double z) {
		RandomSource random = level.getRandom();
		BlockPos pos = BlockPos.containing(x, y + 0.05, z);
		if (!level.getBlockState(pos).isAir()) {
			pos = pos.above();
		}
		if (!level.getBlockState(pos).isAir()) {
			SkyCraft.LOG.info("FNAFSBCraft: no room for a loot chest at {}", pos);
			return;
		}
		var facing = net.minecraft.core.Direction.Plane.HORIZONTAL.getRandomDirection(random);
		level.setBlock(pos, Blocks.CHEST.defaultBlockState().setValue(ChestBlock.FACING, facing), 3);
		if (!(level.getBlockEntity(pos) instanceof ChestBlockEntity chest)) {
			return;
		}
		List<ItemStack> loot = roll(level, random);
		int slot = 0;
		for (ItemStack stack : loot) {
			// Spread about the chest, like Minecraft's loot.
			int at = random.nextInt(chest.getContainerSize());
			for (int tries = 0; tries < 27 && !chest.getItem(at).isEmpty(); tries++) {
				at = (at + 1) % chest.getContainerSize();
			}
			chest.setItem(at, stack);
			slot++;
		}
		SkyCraft.LOG.info("FNAFSBCraft: a loot chest at {} with {} things", pos, slot);
		SkyCraft.award(level.getServer(), "loot");
	}

	private static List<ItemStack> roll(ServerLevel level, RandomSource random) {
		List<ItemStack> out = new ArrayList<>();
		// Getting by.
		Object[][] common = {
			{ Items.BREAD, 1, 4 }, { Items.COOKED_BEEF, 1, 3 }, { Items.TORCH, 4, 12 }, { Items.IRON_INGOT, 1, 5 }, { Items.COBBLESTONE, 8, 32 },
			{ Items.OAK_PLANKS, 8, 24 }, { Items.ARROW, 4, 16 }, { Items.STRING, 2, 6 }, { Items.REDSTONE, 2, 8 }, { Items.GLOWSTONE_DUST, 2, 8 },
		};
		// For fun with the animatronics.
		Object[][] fun = {
			{ Items.CARVED_PUMPKIN, 1, 1 }, { Items.ICE, 1, 3 }, { Items.PACKED_ICE, 1, 2 }, { Items.LEAD, 1, 2 }, { Items.SHEARS, 1, 1 },
			{ Items.SLIME_BLOCK, 1, 4 }, { Items.HONEY_BLOCK, 1, 4 }, { Items.TNT, 1, 3 }, { Items.SADDLE, 1, 1 }, { Items.SHIELD, 1, 1 },
			{ Items.SPYGLASS, 1, 1 }, { Items.IRON_BLOCK, 1, 4 }, { Items.ANVIL, 1, 1 }, { Items.CHICKEN_SPAWN_EGG, 1, 2 },
			{ Items.OAK_BOAT, 1, 1 }, { Items.MINECART, 1, 1 }, { Items.RAIL, 4, 12 }, { Items.LEVER, 1, 3 }, { Items.STONE_BUTTON, 1, 3 },
			{ Items.REDSTONE_BLOCK, 1, 2 }, { Items.REDSTONE_TORCH, 1, 4 }, { Items.STONE_PRESSURE_PLATE, 1, 3 }, { Items.IRON_DOOR, 1, 2 },
			{ Items.OAK_TRAPDOOR, 2, 6 }, { Items.OAK_FENCE, 2, 8 }, { Items.WATER_BUCKET, 1, 1 }, { Items.GOLDEN_CARROT, 2, 6 },
			{ Items.DISPENSER, 1, 1 }, { Items.LANTERN, 1, 4 }, { Items.ENDER_PEARL, 1, 3 },
		};
		int commons = 3 + random.nextInt(4);
		for (int i = 0; i < commons; i++) {
			out.add(stack(common[random.nextInt(common.length)], random));
		}
		int funs = 1 + random.nextInt(2);
		for (int i = 0; i < funs; i++) {
			out.add(stack(fun[random.nextInt(fun.length)], random));
		}
		if (random.nextFloat() < 0.08F) {
			out.add(new ItemStack(Items.ELYTRA));
		}
		// Rarer: things that do a trick on their own.
		if (random.nextFloat() < 0.15F) {
			var tag = new ItemStack(Items.NAME_TAG);
			tag.set(DataComponents.CUSTOM_NAME, Component.literal(random.nextBoolean() ? "jeb_" : "Dinnerbone"));
			out.add(tag);
		}
		if (random.nextFloat() < 0.1F) {
			out.add(enchanted(level, Items.IRON_BOOTS, net.minecraft.world.item.enchantment.Enchantments.FROST_WALKER, 1));
		}
		if (random.nextFloat() < 0.12F) {
			var potion = new ItemStack(Items.POTION);
			potion.set(DataComponents.POTION_CONTENTS, new net.minecraft.world.item.alchemy.PotionContents(net.minecraft.world.item.alchemy.Potions.NIGHT_VISION));
			out.add(potion);
		}
		// With the experiment on, a Void Filler to close the holes again.
		if (SkyCraft.experiments(level.getServer()) && random.nextFloat() < 0.2F) {
			out.add(new ItemStack(SkyCraft.VOID_FILLER));
		}
		if (random.nextFloat() < 0.15F) {
			out.add(new ItemStack(Items.DIAMOND, 1 + random.nextInt(2)));
		}
		// The book of a secret: in two chests out of five, with what it takes to try it.
		if (random.nextFloat() < 0.4F) {
			int secret = random.nextInt(SECRETS.length);
			out.add(book(SECRETS[secret]));
			out.addAll(toolsFor(secret));
		}
		return out;
	}

	/** What each secret (same order as SECRETS) needs, so the book can be tried straight away. */
	private static List<ItemStack> toolsFor(int secret) {
		return switch (secret) {
			case 0 -> List.of(new ItemStack(Items.CARVED_PUMPKIN), new ItemStack(Items.SHEARS));
			case 1 -> List.of(new ItemStack(Items.ICE, 2), new ItemStack(Items.SHEARS));
			case 2 -> List.of(new ItemStack(Items.ELYTRA), new ItemStack(Items.SHEARS));
			case 3 -> List.of(new ItemStack(Items.LEAD, 2), new ItemStack(Items.OAK_FENCE, 4));
			case 4 -> List.of(new ItemStack(Items.WATER_BUCKET));
			case 5 -> List.of(new ItemStack(Items.CHICKEN_SPAWN_EGG));
			case 6 -> {
				var tag = new ItemStack(Items.NAME_TAG);
				tag.set(DataComponents.CUSTOM_NAME, Component.literal("Dinnerbone"));
				yield List.of(tag);
			}
			case 7 -> List.of(new ItemStack(Items.SADDLE));
			case 8 -> List.of(new ItemStack(Items.SHIELD));
			case 9 -> List.of(new ItemStack(Items.CARVED_PUMPKIN));
			case 10 -> List.of(new ItemStack(Items.IRON_BLOCK, 4), new ItemStack(Items.CARVED_PUMPKIN));
			case 11 -> List.of(new ItemStack(dev.skycraft.SkyCraft.VOID_FILLER), new ItemStack(Items.IRON_PICKAXE));
			default -> List.of();
		};
	}

	private static ItemStack enchanted(ServerLevel level, net.minecraft.world.item.Item item,
		net.minecraft.resources.ResourceKey<net.minecraft.world.item.enchantment.Enchantment> enchantment, int strength) {
		var stack = new ItemStack(item);
		level.registryAccess().lookupOrThrow(net.minecraft.core.registries.Registries.ENCHANTMENT).get(enchantment)
			.ifPresent(holder -> stack.enchant(holder, strength));
		return stack;
	}

	private static ItemStack stack(Object[] entry, RandomSource random) {
		int min = (int) entry[1], max = (int) entry[2];
		return new ItemStack((net.minecraft.world.item.Item) entry[0], min + random.nextInt(max - min + 1));
	}

	/** Who signs the mod's books. */
	public static final String AUTHOR = "Mielesgames";

	/**
	 * The book every player gets once: how to get going as Steve in the Pizzaplex, starting with
	 * the first vent (Shift in front of it makes Steve crawl, and the vent is too low to walk).
	 */
	public static ItemStack guide() {
		var book = new ItemStack(Items.WRITTEN_BOOK);
		String[] texts = {
			"Welcome to the Pizzaplex, Steve!\n\nYou are too tall for the vents. Hold Shift in front of one and walk in: you crawl, and you fit.\n\n(A trapdoor works too, like anywhere in Minecraft.)",
			"F6 switches between Steve and Gregory.\n\nRight-click doors and switches to use them, as Gregory would.\n\nEscape opens the menu, with your advancements.",
			"The animatronics hear your steps. Sneak to be quiet, and wear a carved pumpkin so they see you less far.\n\nLook out for chests in rooms you haven't been to.",
			"Commands:\n/tp atrium\n/freddy\n/summon chica\n/repairworld\n/experiment\n\nHave fun!",
		};
		List<Filterable<Component>> pages = new ArrayList<>();
		for (String text : texts) {
			pages.add(Filterable.passThrough(Component.literal(text)));
		}
		book.set(DataComponents.WRITTEN_BOOK_CONTENT, new WrittenBookContent(Filterable.passThrough("Steve's Survival Guide"), AUTHOR, 0, pages, true));
		return book;
	}

	private static ItemStack book(String[] secret) {
		var book = new ItemStack(Items.WRITTEN_BOOK);
		List<Filterable<Component>> pages = new ArrayList<>();
		pages.add(Filterable.passThrough(Component.literal(secret[1])));
		book.set(DataComponents.WRITTEN_BOOK_CONTENT,
			new WrittenBookContent(Filterable.passThrough(secret[0]), AUTHOR, 0, pages, true));
		return book;
	}
}
