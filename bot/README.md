# Unishade bot

The Discord bot for the Unishade server. Mention it with a question and it answers from the docs and from messages the admins picked. For server admins, and for members with the role in `TEAM_ROLE_ID`, it answers anything.

## Context

The bot always reads the docs in `website/src/content/docs`. To give it more, such as an announcement, right-click a message and pick **Apps > Add to context**. **Remove from context** takes it out again, and `/context` lists what's in. Only members who can manage the server see these.

With the question, the bot also reads the 10 messages before it.

## Running it

```powershell
cd bot
pnpm install
pnpm start
```

Set `DISCORD_TOKEN` to the bot's token and `OPENROUTER_API_KEY` to an OpenRouter API key, either in the environment or in `bot/.env`. `OPENROUTER_MODEL` picks the model, `openai/gpt-oss-20b` if it's not set. The bot also needs **Message Content Intent** turned on in the Discord Developer Portal.
