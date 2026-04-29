# Telegram Bot for Private Channel Applications

This is a Telegram bot built with Python and aiogram that handles applications for joining a private channel. Users can submit their details, and an admin can approve or reject them, sending invite links automatically.

## Features

- User application form with name, username, and reason
- Admin panel for reviewing and approving/rejecting applications
- Automatic invite link generation with expiration
- Database storage using SQLite
- Anti-spam protection (5-second throttle)
- Logging for monitoring

## Setup

1. Clone the repository:
   ```bash
   git clone https://github.com/LUX1984/telegrambot.git
   cd telegrambot
   ```

2. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```

3. Create a `.env` file based on `.env.example` (copy it and fill in your values):
   - `BOT_TOKEN`: Get from @BotFather on Telegram
   - `ADMIN_ID`: Your Telegram user ID (as integer)
   - `CHANNEL_ID`: The ID of your private channel (negative number, e.g., -1001234567890)
   - `INVITE_LINK`: Optional static invite link if bot can't create one
   - `DB_PATH`: Path to database file (default: applications.db)

4. Run the bot:
   ```bash
   python bot.py
   ```

## Usage

### For Users
- Start the bot with `/start`
- Fill out the application form
- Wait for admin approval

### For Admins
- `/list`: View pending applications
- `/approve <user_id>`: Approve an application
- `/reject <user_id>`: Reject an application
- `/stats`: View statistics

## Requirements

- Python 3.10+
- Telegram Bot Token
- Private Channel with bot as admin

## License

[Specify license if any]