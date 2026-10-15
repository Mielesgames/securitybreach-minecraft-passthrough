package dev.skycraft.client;

import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/**
 * Security Breach's pause menu as Steve sees it (Escape in Minecraft mode). The game is paused
 * while it is open (the host sees kMcPauseMenu); the buttons that belong to the game are done
 * by the host (EV_MENU). Minecraft's own "Save and Quit" is left out: the world is the link.
 */
public final class SecurityBreachMenu extends Screen {
	public SecurityBreachMenu() {
		super(Component.literal("Game Menu"));
	}

	@Override
	protected void init() {
		int x = this.width / 2 - 102;
		int y = this.height / 4 + 8;
		this.addRenderableWidget(Button.builder(Component.literal("Back to Game"), b -> this.onClose()).bounds(x, y, 204, 20).build());
		this.addRenderableWidget(Button.builder(Component.literal("Advancements"),
			b -> this.minecraft.gui.setScreen(new net.minecraft.client.gui.screens.advancements.AdvancementsScreen(this.minecraft.player.connection.getAdvancements(), this)))
			.bounds(x, y + 24, 98, 20).build());
		this.addRenderableWidget(Button.builder(Component.literal("Statistics"),
			b -> this.minecraft.gui.setScreen(new net.minecraft.client.gui.screens.achievement.StatsScreen(this, this.minecraft.player.getStats())))
			.bounds(x + 106, y + 24, 98, 20).build());
		this.addRenderableWidget(Button.builder(Component.literal("Minecraft Options..."),
			b -> this.minecraft.gui.setScreen(new net.minecraft.client.gui.screens.options.OptionsScreen(this, this.minecraft.options)))
			.bounds(x, y + 48, 98, 20).build());
		this.addRenderableWidget(Button.builder(Component.literal("Game Options..."), b -> this.host(1))
			.bounds(x + 106, y + 48, 98, 20).build());
		this.addRenderableWidget(Button.builder(Component.literal("Quit to Title"), b -> this.host(2)).bounds(x, y + 96, 204, 20).build());
		this.addRenderableWidget(Button.builder(Component.literal("Quit Game"), b -> this.host(3)).bounds(x, y + 120, 204, 20).build());
	}

	/** A button of the game's: the host does it (1 the game's own menu, 2 to the title, 3 quit). */
	private void host(int action) {
		SkyLink.pushEvent(Proto.EV_MENU, 0, 0, 0, 0, 0, action);
		this.onClose();
	}

	@Override
	public void extractRenderState(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
		super.extractRenderState(graphics, mouseX, mouseY, partialTick);
		graphics.centeredText(this.font, this.title, this.width / 2, this.height / 4 - 12, 0xFFFFFFFF);
	}

	@Override
	public boolean isPauseScreen() {
		return true;
	}
}
