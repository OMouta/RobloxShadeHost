import {
  ApplicationCommandType,
  ContextMenuCommandBuilder,
  InteractionContextType,
  MessageFlags,
  PermissionFlagsBits,
  SlashCommandBuilder,
  type Interaction,
} from "discord.js";
import { removeMessage, saveMessage, savedMessages } from "./context.ts";

const add = "Add to context";
const remove = "Remove from context";

const messageCommand = (name: string) =>
  new ContextMenuCommandBuilder()
    .setName(name)
    .setType(ApplicationCommandType.Message)
    .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
    .setContexts(InteractionContextType.Guild);

// Discord shows these to members who can manage the server. Server Settings > Integrations changes who that is.
export const commands = [
  messageCommand(add),
  messageCommand(remove),
  new SlashCommandBuilder()
    .setName("context")
    .setDescription("List the messages the bot answers from")
    .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
    .setContexts(InteractionContextType.Guild),
];

const ephemeral = (content: string) => ({ content, flags: MessageFlags.Ephemeral }) as const;

function list(): string {
  const lines = savedMessages().map(({ url, posted, text }) => `- [${posted}](<${url}>): ${text.replace(/\s+/g, " ").slice(0, 60)}`);
  if (!lines.length) return "No messages in the context. The docs are always included.";
  // A message holds 2000 characters.
  let shown = 0;
  let length = 0;
  while (shown < lines.length && length + lines[shown].length < 1900) length += lines[shown++].length + 1;
  const rest = lines.length - shown;
  return lines.slice(0, shown).join("\n") + (rest ? `\nand ${rest} more` : "");
}

export async function handleInteraction(interaction: Interaction) {
  if (!interaction.inCachedGuild()) return;

  if (interaction.isChatInputCommand()) {
    await interaction.reply(ephemeral(list()));
    return;
  }
  if (!interaction.isMessageContextMenuCommand()) return;

  const message = interaction.targetMessage;
  if (interaction.commandName === remove) {
    await interaction.reply(ephemeral(removeMessage(message.id) ? "Removed from the context." : "That message isn't in the context."));
    return;
  }
  const text = [message.cleanContent, ...message.embeds.flatMap((embed) => [embed.title, embed.description])].filter(Boolean).join("\n\n");
  if (!text) {
    await interaction.reply(ephemeral("That message has no text."));
    return;
  }
  const where = interaction.channel ? `#${interaction.channel.name}` : "Discord";
  const posted = `${where}, ${message.createdAt.toISOString().slice(0, 10)}`;
  // Adding a message again, such as after it was edited, replaces what was saved.
  const replaced = saveMessage({ id: message.id, url: message.url, posted, text });
  await interaction.reply(ephemeral(replaced ? "Updated in the context." : "Added to the context."));
}
