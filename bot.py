"""
Telegram-бот для приёма заявок в приватный канал.
Стек: Python 3.10+, aiogram 3.x, aiosqlite.
"""

import asyncio
import logging
import os
import signal
from datetime import datetime, timedelta, timezone
from typing import Optional, Dict

import asyncpg
from aiogram import Bot, Dispatcher, types, F
from aiogram.filters import Command
from aiogram.types import InlineKeyboardMarkup, InlineKeyboardButton, CallbackQuery
from aiogram.fsm.storage.memory import MemoryStorage
from aiogram.utils.deep_linking import create_start_link
from dotenv import load_dotenv

# ---------- Загрузка конфигурации ----------
# Загружаем .env только локально (не на Railway)
if not os.getenv("RAILWAY_ENVIRONMENT"):
    load_dotenv()

BOT_TOKEN: str = os.getenv("BOT_TOKEN")
ADMIN_ID: int = int(os.getenv("ADMIN_ID")) if os.getenv("ADMIN_ID") else None          # Telegram ID администратора
CHANNEL_ID: int = int(os.getenv("CHANNEL_ID")) if os.getenv("CHANNEL_ID") else None      # ID приватного канала (например, -1001234567890)
INVITE_LINK: Optional[str] = os.getenv("INVITE_LINK")  # Резервная ссылка, если бот не может создать приглашение
DATABASE_URL: str = os.getenv("DATABASE_PUBLIC_URL")  # URL для подключения к БД

if not BOT_TOKEN or ADMIN_ID is None or CHANNEL_ID is None or not DATABASE_URL:
    raise EnvironmentError("BOT_TOKEN, ADMIN_ID, CHANNEL_ID и DATABASE_URL должны быть установлены в переменных окружения")

# ---------- Логирование ----------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    handlers=[
        logging.FileHandler("bot.log", encoding="utf-8"),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# ---------- Инициализация бота и диспетчера ----------
bot = Bot(token=BOT_TOKEN)
dp = Dispatcher(storage=MemoryStorage())

# ---------- Работа с БД ----------
pool = None

async def get_db():
    """Возвращает пул соединений с БД."""
    global pool
    if pool is None:
        pool = await asyncpg.create_pool(DATABASE_URL)
    return pool

async def init_db():
    """Создание таблицы, если её нет."""
    pool = await get_db()
    async with pool.acquire() as conn:
        await conn.execute("""
            CREATE TABLE IF NOT EXISTS applications (
                id SERIAL PRIMARY KEY,
                user_id BIGINT UNIQUE,
                name TEXT NOT NULL,
                username TEXT,
                reason TEXT,
                status TEXT DEFAULT 'pending',
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                invite_link TEXT
            )
        """)



# ---------- Защита от спама ----------
# Простейшая реализация: 5 секунд между любыми сообщениями от одного пользователя
user_last_message: Dict[int, float] = {}

async def throttle_check(message: types.Message) -> bool:
    """Возвращает True, если сообщение можно обработать (прошло >= 5 сек с предыдущего)."""
    user_id = message.from_user.id
    now = datetime.now().timestamp()
    last = user_last_message.get(user_id, 0)
    if now - last < 5:
        await message.answer("⚠️ Слишком быстро! Подождите 5 секунд.")
        return False
    user_last_message[user_id] = now
    return True

# ---------- Вспомогательные функции ----------
async def create_invite_for_user(user_id: int) -> Optional[str]:
    """
    Создаёт уникальную пригласительную ссылку в канал с лимитом 1 использование и сроком 7 дней.
    Если бот не может создать ссылку (не хватает прав), возвращает статическую ссылку из окружения.
    """
    expire_date = datetime.now(timezone.utc) + timedelta(days=7)
    try:
        invite = await bot.create_chat_invite_link(
            chat_id=CHANNEL_ID,
            member_limit=1,
            expire_date=expire_date,
            creates_join_request=False
        )
        logger.info(f"Создана invite-ссылка для user_id={user_id}: {invite.invite_link}")
        return invite.invite_link
    except Exception as e:
        logger.warning(f"Не удалось создать invite-ссылку: {e}. Использую статическую ссылку.")
        return INVITE_LINK

async def send_invite_to_user(user_id: int, invite_link: str):
    """Отправляет пользователю сообщение с приглашением."""
    try:
        await bot.send_message(
            user_id,
            f"✅ Ваша заявка одобрена! Вот ссылка для вступления:\n"
            f"{invite_link}\n"
            f"Ссылка действительна 7 дней и только для одного входа."
        )
    except Exception as e:
        logger.error(f"Ошибка отправки ссылки пользователю {user_id}: {e}")

# ---------- Обработчики команд ----------

@dp.message(Command("admin"), F.func(is_admin))
async def cmd_admin(message: types.Message):
    keyboard = InlineKeyboardMarkup(inline_keyboard=[
        [InlineKeyboardButton(text="📋 List Applications", callback_data="list")],
        [InlineKeyboardButton(text="📊 Statistics", callback_data="stats")]
    ])
    await message.answer("🔧 Admin Panel:", reply_markup=keyboard)

@dp.message(Command("start"))
async def cmd_start(message: types.Message):
    if not await throttle_check(message):
        return

    user_id = message.from_user.id
    user = message.from_user
    name = user.first_name or "Unknown"
    username = user.username

    async with (await get_db()).acquire() as db:
        try:
            await db.execute(
                "INSERT INTO applications (user_id, name, username) VALUES ($1, $2, $3)",
                user_id, name, username
            )
        except asyncpg.UniqueViolationError:
            await message.answer("👋 Вы уже подавали заявку. Ожидайте решения администратора.")
            return

    await message.answer("👋 Добро пожаловать! Ваша заявка на вступление в приватный канал принята автоматически. Ожидайте одобрения администратора.")


            return

    # Автоматически создаём пригласительную ссылку и сохраняем в БД
    invite_link = await create_invite_for_user(user_id)
    if invite_link:
        async with (await get_db()).acquire() as db:
            await db.execute("UPDATE applications SET invite_link = $1 WHERE user_id = $2", invite_link, user_id)
        await send_invite_to_user(user_id, invite_link)
    else:
        # если ссылка не создалась и статической нет
        logger.error(f"Не удалось получить пригласительную ссылку для user_id={user_id}")
        await message.answer("❌ Произошла ошибка при создании приглашения. Администратор свяжется с вами вручную.")

    await message.answer("✅ Заявка принята! Приглашение отправлено вам в личные сообщения.")
    await state.clear()

# ---------- Административные команды ----------

def is_admin(message_or_callback) -> bool:
    if hasattr(message_or_callback, 'from_user'):
        return message_or_callback.from_user.id == ADMIN_ID
    return False

@dp.callback_query(F.data == "list")
async def callback_list(callback: CallbackQuery):
    if not is_admin(callback):
        await callback.answer("Access denied.")
        return

    async with (await get_db()).acquire() as db:
        rows = await db.fetch("SELECT id, user_id, name, username FROM applications WHERE status = $1", 'pending')

    if not rows:
        await callback.answer("📋 Нет заявок в ожидании.")
        return

    text = "<b>📋 Ожидающие заявки:</b>\n\n"
    keyboard = []
    for row in rows:
        id_, uid, name, username = row['id'], row['user_id'], row['name'], row['username']
        username_str = f"@{username}" if username else "не указан"
        text += f"ID: {id_}, User: {uid}, Name: {name}, Username: {username_str}\n\n"
        keyboard.append([
            InlineKeyboardButton(text=f"✅ Approve {id_}", callback_data=f"approve_{uid}"),
            InlineKeyboardButton(text=f"❌ Reject {id_}", callback_data=f"reject_{uid}")
        ])

    await callback.message.edit_text(text, reply_markup=InlineKeyboardMarkup(inline_keyboard=keyboard), parse_mode="HTML")

@dp.callback_query(F.data.startswith("approve_"))
async def callback_approve(callback: CallbackQuery):
    if not is_admin(callback):
        await callback.answer("Access denied.")
        return

    user_id = int(callback.data.split("_")[1])

    async with (await get_db()).acquire() as db:
        row = await db.fetchrow("SELECT invite_link, status FROM applications WHERE user_id = $1", user_id)
        if not row or row['status'] != 'pending':
            await callback.answer("❌ Заявка не найдена или уже обработана.")
            return

        invite_link = row['invite_link']
        if not invite_link:
            invite_link = await create_invite_for_user(user_id)
            if not invite_link:
                await callback.answer("❌ Не удалось создать пригласительную ссылку.")
                return
            await db.execute("UPDATE applications SET invite_link = $1 WHERE user_id = $2", invite_link, user_id)

        await db.execute("UPDATE applications SET status = $1 WHERE user_id = $2", 'approved', user_id)

    await send_invite_to_user(user_id, invite_link)
    await callback.answer(f"✅ Заявка пользователя {user_id} одобрена. Ссылка отправлена.")

@dp.callback_query(F.data.startswith("reject_"))
async def callback_reject(callback: CallbackQuery):
    if not is_admin(callback):
        await callback.answer("Access denied.")
        return

    user_id = int(callback.data.split("_")[1])

    async with (await get_db()).acquire() as db:
        row = await db.fetchrow("SELECT status FROM applications WHERE user_id = $1", user_id)
        if not row or row['status'] != 'pending':
            await callback.answer("❌ Заявка не найдена или уже обработана.")
            return

        await db.execute("UPDATE applications SET status = $1 WHERE user_id = $2", 'rejected', user_id)

    try:
        await bot.send_message(user_id, "❌ Ваша заявка была отклонена администратором.")
    except Exception as e:
        logger.warning(f"Не удалось отправить уведомление об отказе пользователю {user_id}: {e}")

    await callback.answer(f"❌ Заявка пользователя {user_id} отклонена. Пользователь уведомлён.")

@dp.callback_query(F.data == "stats")
async def callback_stats(callback: CallbackQuery):
    if not is_admin(callback):
        await callback.answer("Access denied.")
        return

    now = datetime.now()
    today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    week_start = today_start - timedelta(days=today_start.weekday())

    async with (await get_db()).acquire() as db:
        count_today = await db.fetchval("SELECT COUNT(*) FROM applications WHERE created_at >= $1", today_start.isoformat())
        count_week = await db.fetchval("SELECT COUNT(*) FROM applications WHERE created_at >= $1", week_start.isoformat())
        count_pending = await db.fetchval("SELECT COUNT(*) FROM applications WHERE status = $1", 'pending')

    text = (
        f"📊 Статистика:\n"
        f"• За сегодня: {count_today}\n"
        f"• За неделю: {count_week}\n"
        f"• В ожидании: {count_pending}"
    )
    await callback.message.edit_text(text)

# ---------- Обработка исключений ----------
@dp.errors()
async def errors_handler(update: types.Update, exception: Exception):
    logger.error(f"Unhandled exception: {exception}", exc_info=True)
    return True  # подавляем падение бота

# ---------- Запуск и graceful shutdown ----------
async def main():
    # Инициализация БД
    await init_db()
    logger.info("База данных инициализирована")

    # Запуск поллинга
    try:
        logger.info("Бот запущен")
        await dp.start_polling(bot)
    finally:
        await bot.session.close()
        logger.info("Бот остановлен")

if __name__ == "__main__":
    asyncio.run(main())
# telegrambot
# telegrambot
