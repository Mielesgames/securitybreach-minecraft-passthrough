package dev.skycraft.client;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;

/**
 * The end portal's ending (Security Breach): Mielesgames' pictures, full screen, one after the
 * other, then the game goes back to its title. The game is paused under it (the host sees it as
 * the pause menu) and plays the music. Escape skips to the title.
 */
public final class EndingScreen extends Screen {
	/** How long each picture stays, in ms: the story, the closing words, the last one. */
	private static final int[] DURATIONS = { 4000, 4000, 4000, 1333, 1333, 1834, 5000 };
	private static final Identifier[] FRAMES = new Identifier[DURATIONS.length];

	static {
		for (int i = 0; i < FRAMES.length; i++) {
			FRAMES[i] = Identifier.fromNamespaceAndPath("skycraft", "textures/gui/ending/ending_0" + (i + 1) + ".png");
		}
	}

	private long start = -1;
	private boolean done;

	public EndingScreen() {
		super(Component.literal("The End"));
	}

	/** Which picture shows now; FRAMES.length once the last one is over. */
	private int frame() {
		long now = System.currentTimeMillis();
		if (this.start < 0) {
			this.start = now;
		}
		long t = now - this.start;
		int frame = 0;
		while (frame < DURATIONS.length && t >= DURATIONS[frame]) {
			t -= DURATIONS[frame];
			frame++;
		}
		return frame;
	}

	@Override
	public void tick() {
		if (this.frame() >= FRAMES.length) {
			this.finish();
		}
	}

	/** To the game's title screen (EV_MENU 2), once. */
	private void finish() {
		if (this.done) {
			return;
		}
		this.done = true;
		SkyLink.pushEvent(Proto.EV_MENU, 0, 0, 0, 0, 0, 2);
		SkyCraft.LOG.info("FNAFSBCraft: the ending is over, back to the title");
		this.minecraft.gui.setScreen(null);
	}

	@Override
	public void onClose() {
		this.finish();
	}

	@Override
	public void extractBackground(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
	}

	@Override
	public void extractRenderState(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
		int frame = Math.min(this.frame(), FRAMES.length - 1);
		graphics.fill(0, 0, this.width, this.height, 0xFF000000);
		// 16:9, as big as fits, black bars around it.
		int w = this.width;
		int h = this.width * 9 / 16;
		if (h > this.height) {
			h = this.height;
			w = this.height * 16 / 9;
		}
		graphics.blit(RenderPipelines.GUI_TEXTURED, FRAMES[frame], (this.width - w) / 2, (this.height - h) / 2, 0, 0, w, h, 1920, 1080, 1920, 1080);
	}

	@Override
	public boolean isPauseScreen() {
		return true;
	}
}
