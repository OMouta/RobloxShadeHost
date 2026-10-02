import { Client, Events, GatewayIntentBits, MessageFlags, Partials, PermissionFlagsBits, type Message } from "discord.js";
import { answer } from "./answer.ts";
import { commands, handleInteraction } from "./commands.ts";
import { removeMessage } from "./context.ts";
import { limit, maxQuestions } from "./limit.ts";

const token = process.env.DISCORD_TOKEN;
if (!token) throw new Error("DISCORD_TOKEN is not set");
// Members with this role get an answer to anything, like admins do.
const teamRoleId = process.env.TEAM_ROLE_ID;

// How many messages before the mention are sent along, so a question asked over several messages reads as one.
const historyLength = 10;
// Long earlier messages are cut, so ten of them can't cost more than the question itself.
const maxEarlierLength = 400;

function onTeam(message: Message<true>): boolean {
  const { member } = message;
  if (!member) return false;
  return member.permissions.has(PermissionFlagsBits.Administrator) || (!!teamRoleId && member.roles.cache.has(teamRoleId));
}

const nameOf = (message: Message<true>) =>
  message.author.id === message.client.user.id ? "You" : (message.member?.displayName ?? message.author.displayName);

function cut(text: string): string {
  return text.length > maxEarlierLength ? `${text.slice(0, maxEarlierLength)}...` : text;
}

async function handleMessage(message: Message) {
  if (message.author.bot || message.system || !message.inGuild()) return;
  // A reply to one of the bot's messages mentions it too, unless the person turned the ping off.
  if (!message.mentions.has(message.client.user, { ignoreEveryone: true, ignoreRoles: true })) return;

  const team = onTeam(message);
  if (!team) {
    const verdict = limit(message.author.id);
    if (verdict === "warn") await message.reply(`You can ask ${maxQuestions} questions a minute. Try again in a bit.`);
    if (verdict !== "answer") return;
  }
  await message.channel.sendTyping();

  const earlier = await message.channel.messages.fetch({ limit: historyLength, before: message.id });
  const parts: string[] = [];
  if (message.channel.isThread()) parts.push(`Thread title: ${message.channel.name}`);
  if (earlier.size) {
    const lines = [...earlier.values()].reverse().map((previous) => `${nameOf(previous)}: ${cut(previous.cleanContent)}`);
    parts.push(`Earlier messages, oldest first:\n${lines.join("\n")}`);
  }
  parts.push(`Message that mentions you, from ${nameOf(message)}:\n${message.cleanContent}`);

  // Discord shows "is typing" for 10 seconds and a slow model takes longer, so it's sent again until the answer is in.
  // A failed one only costs the indicator.
  const typing = setInterval(() => void message.channel.sendTyping().catch(() => {}), 8_000);
  let reply: string;
  try {
    reply = await answer(parts.join("\n\n"), team);
  } catch (error) {
    console.error(`Could not answer message ${message.id}:`, error);
    reply = "I couldn't answer that right now. Try again in a minute.";
  } finally {
    clearInterval(typing);
  }
  await message.reply({
    content: reply.slice(0, 2000),
    // The answer comes from a model reading what users typed, so it pings nobody but the person asking.
    allowedMentions: { parse: [], repliedUser: true },
    flags: MessageFlags.SuppressEmbeds,
  });
}

const client = new Client({
  intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildMessages, GatewayIntentBits.MessageContent],
  // Without this, deleting a message the bot hasn't seen since it started goes unnoticed.
  partials: [Partials.Message],
});

client.once(Events.ClientReady, async (ready) => {
  await ready.application.commands.set(commands);
  console.log(`Logged in as ${ready.user.tag}`);
});

// One failed message or command is logged and doesn't take the bot down.
client.on(Events.MessageCreate, (message) => {
  handleMessage(message).catch((error) => console.error(`Could not reply to message ${message.id}:`, error));
});
client.on(Events.InteractionCreate, (interaction) => {
  handleInteraction(interaction).catch((error) => console.error(`Could not handle interaction ${interaction.id}:`, error));
});
// A deleted message can't be picked for Remove from context any more, so it leaves the context with it.
client.on(Events.MessageDelete, (message) => {
  removeMessage(message.id);
});

await client.login(token);
