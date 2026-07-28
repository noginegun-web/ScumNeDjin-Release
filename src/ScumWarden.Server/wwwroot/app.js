const STATIC_ASSET_VERSION = '20260719_rental_state_normalization';

const state = {
  apiKey: localStorage.getItem('nedjin.apiKey.v2') || '',
  apiBase: localStorage.getItem('nedjin.apiBase.v2') || '',
  page: 'servers',
  servers: [],
  players: [],
  squads: [],
  chat: [],
  kills: [],
  homes: [],
  welcomeTimers: [],
  welcomeOptimisticTimers: [],
  welcomeClaims: [],
  welcomeTimersLoaded: false,
  welcomeTimersLoading: false,
  downloads: [],
  items: [],
  vehicles: [],
  vehicleRentals: [],
  skillCatalog: [],
  itemIcons: {},
  itemIconOverrides: {},
  itemIconLookup: new Map(),
  itemIconAssetsLoaded: false,
  itemIconAssetsLoading: false,
  itemIconAssetsPromise: null,
  generatedIconCache: new Map(),
  itemBatch: [],
  wargmItems: [],
  map: null,
  modules: [],
  selectedModule: null,
  moduleDraft: null,
  moduleConfigBaselines: {},
  mapZoom: 1,
  mapSelection: null,
  selectedPlayerTarget: null,
  playerInventory: {},
  playerFacts: {},
  chatAuxLoaded: {},
  chatAuxLoading: {},
  playersRefreshInFlight: null,
  statusRefreshInFlight: null,
  playersLastRefreshedAt: 0,
  hiddenRuntimePlayers: 0,
  runtimeOnlyPlayers: 0,
  actionLog: [],
  lastCatalogInput: null,
  catalogMode: 'items',
  catalogSearch: '',
  catalogCategory: '',
  catalogSelected: null,
  serviceTab: 'player',
  wargmConfigTab: 'settings',
  wargmRuleEditIndex: null,
  wargmRuleSearch: '',
  vipConfigTab: 'settings',
  selectedSquadKey: '',
  selectedKillKey: '',
  simpleModuleTabs: {},
  simpleModuleEditIndex: null,
  simpleModuleEditArrayKey: null,
  simpleModuleEditDraft: null,
  simpleModuleEditIsNew: false,
  simpleSettingEditPath: null,
  simpleModuleSearch: '',
  schedulerConfigTab: 'settings',
  schedulerJobIndex: null,
  schedulerJobEditIndex: null,
  schedulerJobEditIsNew: false,
  schedulerJobSearch: '',
  worldEditLastResult: null,
  language: localStorage.getItem('nedjin.language.v1') || 'ru',
  lastStatus: null,
  lastDiagnosticsProblem: null,
  serverConfigs: [],
  selectedServerConfig: '',
  serverConfigExpanded: false,
  serverConfigLoadVersion: 0,
  serverConfigLoading: false,
  serverConfigLoadedName: '',
  serverConfigLoadedContent: null
};

function normalizeApiBase(input) {
  let value = String(input || '').trim();
  if (!value) return '';
  if (!/^https?:\/\//i.test(value)) value = `http://${value}`;
  try {
    const url = new URL(value, window.location.origin);
    url.hash = '';
    url.search = '';
    url.pathname = url.pathname.replace(/\/(?:panel|index)\.html$/i, '/');
    url.pathname = url.pathname.replace(/\/+$/, '');
    return url.pathname && url.pathname !== '/' ? `${url.origin}${url.pathname}` : url.origin;
  } catch (_) {
    return '';
  }
}

state.apiBase = normalizeApiBase(state.apiBase);

const els = {};
for (const el of document.querySelectorAll('[id]')) els[el.id] = el;
if (!els.saveApiKey && els.connectBtn) els.saveApiKey = els.connectBtn;
if (els.serverHost && !els.serverHost.value) els.serverHost.value = state.apiBase || window.location.origin;

const panelI18n = {
  ru: {
    platform: 'Платформа',
    bridge: 'мост',
    players: 'игроки',
    checking: 'проверка...',
    online: 'в сети',
    offline: 'не в сети',
    noConnection: 'нет связи',
    readable: 'читается',
    missing: 'не найдена',
    connectedHost: 'подключённая площадка',
    pages: {
      servers: 'Обзор сервера',
      players: 'Игроки',
      map: 'Живая карта',
      squads: 'Отряды',
      chat: 'Чат',
      kills: 'Убийства',
      modules: 'Плагины',
      services: 'Сервисы',
      console: 'Консоль',
      downloads: 'Загрузки',
      diagnostics: 'Диагностика'
    }
  },
  en: {
    platform: 'Platform',
    bridge: 'bridge',
    players: 'players',
    checking: 'checking...',
    online: 'online',
    offline: 'offline',
    noConnection: 'no connection',
    readable: 'readable',
    missing: 'missing',
    connectedHost: 'connected host',
    pages: {
      servers: 'Server overview',
      players: 'Players',
      map: 'Live map',
      squads: 'Squads',
      chat: 'Chat',
      kills: 'Kills',
      modules: 'Plugins',
      services: 'Services',
      console: 'Console',
      downloads: 'Downloads',
      diagnostics: 'Diagnostics'
    }
  }
};

const PANEL_TEXT_EN = {
  'Панель управления': 'Control panel',
  'Подключение': 'Connection',
  'IP:PORT или URL панели': 'IP:PORT or panel URL',
  'Ключ доступа': 'Access key',
  'Ключ доступа не задан.': 'Access key is not set.',
  'Подключиться': 'Connect',
  'Сервер': 'Server',
  'Статус': 'Status',
  'Обзор сервера': 'Server overview',
  'SCUM сервер': 'SCUM server',
  'SCUM серверы': 'SCUM servers',
  'проверка...': 'checking...',
  'Язык панели': 'Panel language',
  'Русский': 'Russian',
  'Серверы': 'Servers',
  'Игроки': 'Players',
  'Отряды': 'Squads',
  'Чат': 'Chat',
  'Убийства': 'Kills',
  'Плагины': 'Plugins',
  'Сервисы': 'Services',
  'Консоль': 'Console',
  'Загрузки': 'Downloads',
  'Файлы пакета': 'Package files',
  'ФАЙЛЫ ПАКЕТА': 'PACKAGE FILES',
  'Обнаружена ошибка SCUM NeDjin': 'SCUM NeDjin issue detected',
  'Скачайте диагностический пакет, откройте тикет в Discord и прикрепите файл вручную.': 'Download the diagnostics package, open a Discord ticket, and attach the file manually.',
  'Открыть тикет': 'Open ticket',
  'Скачать логи': 'Download logs',
  'Команды и статус обрабатываются автоматически. Ключ доступа храните только у администраторов.': 'Commands and status are processed automatically. Keep the access key admin-only.',
  'Панель сервера': 'Server panel',
  'Поддержка автора': 'Support author',
  'Обновить': 'Refresh',
  'Обновить события': 'Refresh events',
  'Подключенные игровые серверы и быстрые действия выбранного экземпляра.': 'Connected game servers and quick actions for the selected instance.',
  'Последние события': 'Recent events',
  'Быстрые действия': 'Quick actions',
  'Сообщение всем игрокам': 'Message all players',
  'Отправить': 'Send',
  '#Announce Перезапуск через 10 минут': '#Announce Restart in 10 minutes',
  'Выполнить': 'Run',
  'Выдать': 'Give',
  'В список': 'To list',
  'Выдать список': 'Give list',
  'Телепорт': 'Teleport',
  'Золото': 'Gold',
  'Создать': 'Create',
  'Транспорт': 'Vehicles',
  'Другое': 'Other',
  'Поиск по имени или SteamID': 'Search by name or SteamID',
  'Сортировка: имя': 'Sort: name',
  'Сортировка: деньги': 'Sort: money',
  'Сортировка: слава': 'Sort: fame',
  'Сортировка: последний вход': 'Sort: last login',
  'Игрок': 'Player',
  'оффлайн': 'offline',
  'онлайн': 'online',
  'SteamID не выбран': 'SteamID not selected',
  'SteamID не найден': 'SteamID not found',
  'Профиль': 'Profile',
  'Скрыть': 'Hide',
  'Выдача предметов': 'Item delivery',
  'ID предмета или поиск по каталогу': 'Item ID or catalog search',
  'Кол-во': 'Qty',
  'Выдать игроку': 'Give to player',
  'Еда': 'Food',
  'Вода': 'Water',
  'Бинт': 'Bandage',
  'Стартпак': 'Starter pack',
  'Статус обновится при открытии игрока.': 'Status updates when the player profile is opened.',
  'Выдать стартпак': 'Give starter pack',
  'Снять стартпак': 'Reset starter pack',
  'Battlepass': 'Battlepass',
  'Выдаётся автоматически после входа. Игрок может проверить статус командой /battlepass.': 'Granted automatically after login. The player can check status with /battlepass.',
  'Перемещение': 'Teleport',
  'Переместить игрока': 'Teleport player',
  'Открыть расширенный телепорт': 'Open advanced teleport',
  'Управление': 'Management',
  'Инвентарь': 'Inventory',
  'Баланс': 'Balance',
  'Навыки и атрибуты': 'Skills and attributes',
  'Транспорт': 'Vehicles',
  'Модерация': 'Moderation',
  'Причина действия': 'Action reason',
  'Кикнуть': 'Kick',
  'Забанить': 'Ban',
  'Живая карта': 'Live map',
  'Live-координаты игроков, транспорта и базовых объектов поверх карты.': 'Live coordinates for players, vehicles, and base objects over the map.',
  'Сброс': 'Reset',
  'Карта SCUM': 'SCUM map',
  'Слои': 'Layers',
  'Сундуки': 'Chests',
  'Флаги': 'Flags',
  'Выбранная точка': 'Selected point',
  'Ничего не выбрано.': 'Nothing selected.',
  'Поиск по отряду или участнику': 'Search by squad or member',
  'Чат сервера': 'Server chat',
  'Глобальный': 'Global',
  'Локальный': 'Local',
  'Отряд': 'Squad',
  'Админ': 'Admin',
  'Команды': 'Commands',
  'Поиск по сообщению или игроку': 'Search by message or player',
  'Отправка': 'Send message',
  'SteamID или имя, пусто = всем': 'SteamID or name, empty = everyone',
  'Убийства сервера': 'Server kills',
  'Экономика': 'Economy',
  'Дом': 'Home',
  'Дома': 'Homes',
  'Магазин': 'Shop',
  'Настройки': 'Settings',
  'Игроки VIP': 'VIP players',
  'VIP игроки': 'VIP players',
  'VIP': 'VIP',
  'Добавить': 'Add',
  'Сохранить': 'Save',
  'Удалить': 'Delete',
  'Редактировать': 'Edit',
  'Отмена': 'Cancel',
  'Закрыть': 'Close',
  'Проверить': 'Check',
  'Запустить': 'Run',
  'Выключено': 'Disabled',
  'Включено': 'Enabled',
  'работает': 'running',
  'нет связи': 'no connection',
  'читается': 'readable',
  'данные доступны': 'data available',
  'события доступны': 'events available',
  'подключённая площадка': 'connected host',
  'подключенная площадка': 'connected host',
  'мост': 'bridge',
  'игроки': 'players',
  'Сейчас серверный лог не подтверждает игроков онлайн.': 'The server log does not currently confirm online players.',
  'Событий пока нет.': 'No events yet.',
  'настроен': 'configured',
  'Ключ': 'Key',
  'Мост': 'Bridge',
  'не задан': 'not set',
  'Все': 'All',
  'Загружаю инвентарь...': 'Loading inventory...',
  'Пусто или слот не сохранён в базе.': 'Empty or the slot is not saved in the database.',
  'Быстрые слоты': 'Quick slots',
  'Экипировка и контейнеры': 'Equipment and containers',
  'Нет записей': 'No records',
  'ручная точка': 'manual point',
  'Цена считается от позиции игрока: укажи онлайн-игрока сверху.': 'The price is calculated from the player position: select an online player above.',
  'Укажи маршрут или координаты точки.': 'Select a route or point coordinates.',
  'Предметы': 'Items',
  'Для кейса добавь несколько предметов в набор. Если набор пуст, выдастся один предмет из поля ID.': 'For a case, add several items to the set. If the set is empty, one item from the ID field is delivered.',
  'Выдача транспорта ставится в live-очередь и выполняется рядом с выбранным игроком.': 'Vehicle delivery is queued live and executed near the selected player.',
  'Вызывает SCUM cargo drop по live-координатам выбранного игрока через ScheduleWorldEvent BP_CargoDropEvent X= Y= Z=.': 'Calls a SCUM cargo drop at the selected player live coordinates through ScheduleWorldEvent BP_CargoDropEvent X= Y= Z=.',
  'Выдаёт VIP-привилегию на указанное число дней. Если у игрока уже есть VIP, срок продлится от текущей даты окончания.': 'Grants VIP privileges for the selected number of days. If the player already has VIP, the expiration date is extended.',
  'Изменяет обычный баланс игрока через безопасный bridge route.': 'Changes the player normal balance through the safe bridge route.',
  'Начисляет или списывает золото игроку через безопасный bridge route.': 'Adds or removes player gold through the safe bridge route.',
  'Начисляет или списывает очки славы поверх текущей суммы.': 'Adds or removes fame points on top of the current value.',
  'Навык': 'Skill',
  'Выбери навык из каталога или введи точное имя, например Handgun или Melee Weapons.': 'Select a skill from the catalog or enter an exact name, for example Handgun or Melee Weapons.',
  'Все навыки': 'All skills',
  'Выдаёт весь список навыков SCUM выбранному игроку. Для полной прокачки оставь уровень 4.': 'Grants the full SCUM skill list to the selected player. Leave level 4 for max training.',
  'Атрибуты': 'Attributes',
  'Укажи четыре значения персонажа: сила, тело, ловкость и интеллект.': 'Set four character values: strength, constitution, dexterity, and intelligence.',
  'Команда': 'Command',
  'Для редких случаев. Используй только проверенные SCUM-команды.': 'For rare cases. Use only verified SCUM commands.',
  'цель не указана': 'target is not set',
  'Выдать предметы': 'Deliver items',
  'Укажи SteamID или имя игрока в верхнем поле.': 'Enter SteamID or player name in the field above.',
  'Добавь предмет в набор или укажи один ID предмета.': 'Add an item to the set or enter one item ID.',
  'Укажи ID транспорта.': 'Enter vehicle ID.',
  'Укажи ненулевую сумму.': 'Enter a non-zero amount.',
  'Укажи срок VIP в днях.': 'Enter VIP duration in days.',
  'Укажи навык.': 'Enter a skill.',
  'Укажи значения атрибутов.': 'Enter attribute values.',
  'Укажи команду.': 'Enter a command.',
  'Убрать': 'Remove',
  'Набор пуст. Добавь предметы для кейса или оставь один ID в поле выше.': 'The set is empty. Add case items or leave one ID in the field above.',
  'Укажи ID предмета.': 'Enter item ID.',
  'Статистика не читается в фоне. Открой вкладку и нажми обновление, когда она нужна.': 'Stats are not read in the background. Open the tab and refresh when needed.',
  'Данные персонажа не читаются в фоне. Открой вкладку и нажми обновить, когда они нужны.': 'Character data is not read in the background. Open the tab and refresh when needed.',
  'Баланс не читается в фоне. Нажми "Обновить данные", когда нужно проверить начисление.': 'Balance is not read in the background. Click "Refresh data" when you need to verify a grant.',
  'Инвентарь не мониторится автоматически. Нажми "Обновить инвентарь", когда он реально нужен.': 'Inventory is not monitored automatically. Click "Refresh inventory" when it is needed.',
  'Сначала выбери игрока.': 'Select a player first.',
  'Диагностика отключена, чтобы не нагружать сервер.': 'Diagnostics are disabled to avoid loading the server.',
  'Чат очищен на экране. На сервере логи не удалялись.': 'Chat was cleared on screen. Server logs were not deleted.',
  'Координаты вставлены в поля телепорта.': 'Coordinates inserted into teleport fields.',
  'Координаты скопированы.': 'Coordinates copied.',
  'Вставь Bot token перед сохранением.': 'Enter the bot token before saving.',
  'Задание выключено. Всё равно запустить его один раз для проверки?': 'This job is disabled. Run it once for testing anyway?',
  'Безопасное обновление выполнено: панель перечитала состояние и конфиги. Серверный процесс не перезапускался.': 'Safe refresh complete: the panel reread state and configs. The server process was not restarted.',
  'Поддержка автора': 'Support author',
  'Перевод по карте Сбербанка': 'Sberbank card transfer',
  'Номер карты можно скопировать и вставить в Сбербанк Онлайн.': 'You can copy the card number and paste it into Sberbank Online.',
  'Сбербанк': 'Sberbank',
  'Скопировать номер карты': 'Copy card number',
  'Панель копирует номер карты и открывает официальный Сбербанк Онлайн. В банке выберите перевод по номеру карты и вставьте номер.': 'The panel copies the card number and opens the official Sberbank Online site. In the bank app, choose card-number transfer and paste the number.',
  'Открыть Сбербанк Онлайн': 'Open Sberbank Online',
  'Копировать карту': 'Copy card',
  'Номер карты Сбербанка скопирован.': 'Sberbank card number copied.',
  'Откройте Сбербанк Онлайн и вставьте номер карты из окна поддержки.': 'Open Sberbank Online and paste the card number from the support window.',
  'Сообщение': 'Message',
  'Очистить чат': 'Clear chat',
  'Очистить логи': 'Clear logs',
  'Очистить убийства': 'Clear kills',
  'Журналы проекта очищены. SCUM.log не изменялся.': 'Project logs were cleared. SCUM.log was not changed.',
  'Поиск по убийце, жертве, оружию': 'Search by killer, victim, or weapon',
  'Обзор денег, банка и платных действий. Начисления выполняются из карточки выбранного игрока.': 'Overview of money, bank, and paid actions. Grants are made from the selected player card.',
  'Обзор': 'Overview',
  'Цены': 'Prices',
  'Кошельки': 'Wallets',
  'Банк': 'Bank',
  'Операции': 'Operations',
  'Деньги и расходы': 'Money and expenses',
  'Баланс игрока': 'Player balance',
  'SteamID или имя': 'SteamID or name',
  'Обычные деньги': 'Normal money',
  'Изменение баланса': 'Balance change',
  'Изменить баланс': 'Change balance',
  'Начислить славу (+/-)': 'Add fame (+/-)',
  'Начислить славу': 'Add fame',
  'Настройки игровых команд, сервисов и интеграций.': 'Game command, service, and integration settings.',
  'Обновить панель': 'Refresh panel',
  'Безопасно перечитать состояние панели без перезапуска сервера': 'Safely reread panel state without restarting the server',
  'Отключить': 'Disable',
  'Модуль': 'Module',
  'Каталог': 'Catalog',
  'Поиск предмета или транспорта': 'Search item or vehicle',
  'Расширенные настройки': 'Advanced settings',
  'Ручные операции': 'Manual operations',
  'Быстрые действия для выбранного игрока. Настройки этих систем находятся во вкладке «Плагины».': 'Quick actions for the selected player. These systems are configured on the Plugins tab.',
  'SteamID или имя игрока для действий ниже': 'SteamID or player name for actions below',
  'Стартовый набор': 'Starter pack',
  'Выдача и сброс WelcomePack для тестов или ручной помощи.': 'Grant and reset WelcomePack for tests or manual help.',
  'Выдать набор': 'Give pack',
  'Сбросить таймеры': 'Reset timers',
  'Домашние точки': 'Home points',
  'Сохранённые точки игрока и быстрый возврат к ним.': 'Saved player points and quick return.',
  'Название точки': 'Point name',
  'Сохранить текущую позицию': 'Save current position',
  'Обновить список': 'Refresh list',
  'Быстрое перемещение': 'Fast travel',
  'Ручной телепорт по маршруту или координатам.': 'Manual teleport by route or coordinates.',
  'Сумма списания (если нужна)': 'Charge amount, if needed',
  'Выбери игрока и маршрут, чтобы увидеть цену.': 'Select player and route to see the price.',
  'Телепортировать': 'Teleport',
  'Аренда транспорта': 'Vehicle rental',
  'Создать арендованный транспорт рядом с игроком.': 'Create a rental vehicle near the player.',
  'Минуты аренды': 'Rental minutes',
  'Выдать транспорт': 'Spawn vehicle',
  'Очистить истёкшую аренду': 'Clean expired rentals',
  'Скан текущего квадрата': 'Current sector scan',
  'Проверка сектора вокруг выбранного игрока.': 'Check the sector around the selected player.',
  'Сканировать игрока': 'Scan player',
  'Wargm выдача': 'Wargm delivery',
  'Цель берется из поля SteamID или имени игрока сверху.': 'The target is taken from the SteamID or player name field above.',
  'Что выдать': 'What to deliver',
  'Предметы / кейс': 'Items / case',
  'Cargo drop на игрока': 'Cargo drop on player',
  'Очки славы': 'Fame points',
  'Предметы или кейс': 'Items or case',
  'Можно добавить несколько строк: всё уйдет одной заявкой.': 'You can add several rows: everything is sent as one request.',
  'ID предмета': 'Item ID',
  'Например Weapon_MK18': 'For example Weapon_MK18',
  'Очистить набор': 'Clear set',
  'ID транспорта': 'Vehicle ID',
  'Например BPC_WolfsWagen': 'For example BPC_WolfsWagen',
  'Сумма / дней VIP': 'Amount / VIP days',
  'Например 1000 или 30': 'For example 1000 or 30',
  'Уровень': 'Level',
  'Опыт': 'Experience',
  'Все навыки сразу': 'All skills at once',
  'Выдаст полный список навыков SCUM по очереди. Обычно ставь уровень 4.': 'Applies the full SCUM skill list sequentially. Usually use level 4.',
  'Сила': 'Strength',
  'Тело': 'Constitution',
  'Ловкость': 'Dexterity',
  'Интеллект': 'Intelligence',
  '#Announce Покупка выдана {name}': '#Announce Purchase delivered to {name}',
  'Синхронизировать покупки': 'Sync purchases',
  'Подтвердить выданные': 'Confirm delivered',
  'Выдать выбранное': 'Deliver selected',
  'Можно отправлять события через webhook или через Discord-бота в выбранные каналы. Токен хранится отдельно в секретном файле.': 'Events can be sent through a webhook or Discord bot to selected channels. The token is stored separately in a secret file.',
  'Куда отправить тест': 'Where to send test',
  'Система': 'System',
  'Входы/выходы': 'Joins/leaves',
  'Бои': 'Combat',
  'Вставь токен бота, если выбран режим Bot Channel': 'Enter bot token if Bot Channel mode is selected',
  'Тестовое сообщение Discord': 'Discord test message',
  'Статус': 'Status',
  'Сохранить токен': 'Save token',
  'Отправить тест': 'Send test',
  'Результат': 'Result',
  'Выдача предметов': 'Item delivery',
  'Количество': 'Quantity',
  'Требуемое право': 'Required permission',
  'Кулдаун, ч': 'Cooldown, h',
  'Сообщение при успехе': 'Success message',
  'Выдавать по одному игроку за раз': 'Deliver to one player at a time',
  'Пауза между предметами, мс': 'Delay between items, ms',
  'Атрибуты по умолчанию': 'Default attributes',
  'Телосложение': 'Constitution',
  'Другое': 'Other',
  'объявление сервера': 'server announcement',
  'игровая очередь': 'game queue',
  'командный канал': 'command channel',
  'Выполняется...': 'Running...',
  'Готово.': 'Done.',
  'Не выполнено.': 'Failed.',
  'Команда отправлена.': 'Command sent.',
  'Операция завершена.': 'Operation finished.',
  'Записей нет.': 'No records.',
  'не AActor': 'not AActor'
};

const PANEL_TEXT_PATTERNS_EN = [
  [/^Платформа\s*\/\s*(.+)$/i, m => `Platform / ${translatePanelText(m[1])}`],
  [/^мост:\s*(.+)$/i, m => `bridge: ${translatePanelText(m[1])}`],
  [/^игроки:\s*(\d+)$/i, m => `players: ${m[1]}`],
  [/^SERVER:[ \t]*(.+)$/i, m => `SERVER: ${m[1]}`],
  [/^STATUS:[ \t]*(.+)$/i, m => `STATUS: ${translatePanelText(m[1])}`],
  [/^SERVER:[ \t]*$/i, () => 'SERVER:'],
  [/^STATUS:[ \t]*$/i, () => 'STATUS:'],
  [/^Цена до "(.+)":\s*(.+)$/i, m => `Price to "${m[1]}": ${m[2]}`],
  [/^Цена до "(.+)":\s*(.+)\.\s*Расстояние:\s*(.+)$/i, m => `Price to "${m[1]}": ${m[2]}. Distance: ${m[3]}`],
  [/^К выдаче:\s*(\d+)\s*строк,\s*(\d+)\s*шт\.\s*Цель:\s*(.+)\.$/i, m => `To deliver: ${m[1]} rows, ${m[2]} pcs. Target: ${translatePanelText(m[3])}.`],
  [/^Выбери предмет или собери набор\.\s*Цель:\s*(.+)\.$/i, m => `Select an item or build a set. Target: ${translatePanelText(m[1])}.`],
  [/^Транспорт:\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `Vehicle: ${translatePanelText(m[1])}. Target: ${translatePanelText(m[2])}.`],
  [/^Cargo drop будет вызван рядом с игроком\.\s*Цель:\s*(.+)\.$/i, m => `Cargo drop will be called near the player. Target: ${translatePanelText(m[1])}.`],
  [/^VIP на\s*(\d+)\s*дней\.\s*Цель:\s*(.+)\.$/i, m => `VIP for ${m[1]} days. Target: ${translatePanelText(m[2])}.`],
  [/^Баланс:\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `Balance: ${m[1]}. Target: ${translatePanelText(m[2])}.`],
  [/^Золото:\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `Gold: ${m[1]}. Target: ${translatePanelText(m[2])}.`],
  [/^Очки славы:\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `Fame points: ${m[1]}. Target: ${translatePanelText(m[2])}.`],
  [/^Навык:\s*(.+),\s*уровень\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `Skill: ${translatePanelText(m[1])}, level ${m[2]}. Target: ${translatePanelText(m[3])}.`],
  [/^Все навыки:\s*уровень\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `All skills: level ${m[1]}. Target: ${translatePanelText(m[2])}.`],
  [/^Атрибуты:\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `Attributes: ${m[1]}. Target: ${translatePanelText(m[2])}.`],
  [/^Команда:\s*(.+)\.\s*Цель:\s*(.+)\.$/i, m => `Command: ${translatePanelText(m[1])}. Target: ${translatePanelText(m[2])}.`],
  [/^Выдать:\s*(.+)$/i, m => `Deliver: ${translatePanelText(m[1])}`],
  [/^Поставить\s*(\d+)\s*навыка\(ов\)\s*в безопасную очередь уровнем\s*(\d+)\?$/i, m => `Put ${m[1]} skill(s) into the safe queue at level ${m[2]}?`],
  [/^Ставлю\s*(\d+)\s*навыка\(ов\)\s*в безопасную очередь\.\s*Сервер выдаст их по одному\.$/i, m => `Putting ${m[1]} skill(s) into the safe queue. The server will apply them one by one.`],
  [/^Исключить\s*(.+)\s*из отряда\?$/i, m => `Remove ${m[1]} from the squad?`],
  [/^SteamID скопирован:\s*(.+)$/i, m => `SteamID copied: ${m[1]}`],
  [/^Сбросить стартовый набор для\s*(.+)\?\s*Игрок сможет получить его заново\.$/i, m => `Reset starter pack for ${m[1]}? The player will be able to claim it again.`],
  [/^Проверяю стартпак$/i, () => 'Checking starter pack'],
  [/^Стартпак получен$/i, () => 'Starter pack received'],
  [/^Проверка\.\.\.$/i, () => 'Checking...'],
  [/^Стартпак не получен$/i, () => 'Starter pack not received'],
  [/^профиль\s*(.+)$/i, m => `profile ${m[1]}`],
  [/^не выбран$/i, () => 'not selected'],
  [/^не указана$/i, () => 'not set'],
  [/^не выбран$/i, () => 'not selected'],
  [/^В очереди:\s*(\d+)(?:\s*из\s*(\d+))?\.$/i, m => m[2] ? `Queued: ${m[1]} of ${m[2]}.` : `Queued: ${m[1]}.`],
  [/^Отправлено:\s*(\d+)\.$/i, m => `Sent: ${m[1]}.`],
  [/^Пакетов:\s*(\d+)\.$/i, m => `Packets: ${m[1]}.`],
  [/^Не получилось:\s*(.+)$/i, m => `Failed: ${m[1]}`],
  [/^Получено записей:\s*(\d+)\.$/i, m => `Records received: ${m[1]}.`],
  [/^Команда отправлена через\s*(.+)\.$/i, m => `Command sent through ${m[1]}.`],
  [/^STATUS:[ \t]*NO CONNECTION$/i, () => 'STATUS: NO CONNECTION'],
  [/^STATUS:[ \t]*ONLINE[ \t]*\((\d+)[ \t]*players\)$/i, m => `STATUS: ONLINE (${m[1]} players)`]
];

const PANEL_TRANSLATE_TEXT_SOURCES = new WeakMap();
const PANEL_TRANSLATABLE_ATTRS = ['placeholder', 'title', 'aria-label'];
const PANEL_TRANSLATE_SKIP = 'script,style,textarea,pre,#moduleConfig,#quickResult,#traceLog,#serverLog,#runtimeLog,#actionLog,.result';
let panelTranslationQueued = false;
let panelTranslationForceQueued = false;
let panelTranslationObserver = null;

function translatePanelText(source) {
  const raw = String(source == null ? '' : source);
  if (state.language !== 'en') return raw;
  const leading = raw.match(/^\s*/)[0] || '';
  const trailing = raw.match(/\s*$/)[0] || '';
  const core = raw.trim();
  if (!core) return raw;
  const compact = core.replace(/\s+/g, ' ');
  const exact = PANEL_TEXT_EN[core] || PANEL_TEXT_EN[compact];
  if (exact) return leading + exact + trailing;
  for (const [pattern, replacer] of PANEL_TEXT_PATTERNS_EN) {
    const match = core.match(pattern);
    if (match) return leading + replacer(match) + trailing;
  }
  return raw;
}

function translatePanelAttr(el, attr) {
  if (!el || !el.getAttribute) return;
  const dataKey = `i18nSource${attr.replace(/[^a-z0-9]/gi, '_')}`;
  let source = el.dataset ? el.dataset[dataKey] : null;
  if (source == null) {
    source = el.getAttribute(attr) || '';
    if (el.dataset) el.dataset[dataKey] = source;
  }
  const next = state.language === 'en' ? translatePanelText(source) : source;
  if ((el.getAttribute(attr) || '') !== next) el.setAttribute(attr, next);
}

function translatePanelNodeText(node) {
  if (!node || node.nodeType !== Node.TEXT_NODE) return;
  const parent = node.parentElement;
  if (!parent || parent.closest(PANEL_TRANSLATE_SKIP)) return;
  const current = node.nodeValue || '';
  if (!current.trim()) return;
  if (!PANEL_TRANSLATE_TEXT_SOURCES.has(node)) {
    PANEL_TRANSLATE_TEXT_SOURCES.set(node, current);
  }
  let source = PANEL_TRANSLATE_TEXT_SOURCES.get(node) || '';
  const previousTranslated = translatePanelText(source);
  if (state.language === 'en' && current !== source && current !== previousTranslated) {
    source = current;
    PANEL_TRANSLATE_TEXT_SOURCES.set(node, source);
  }
  if (state.language !== 'en' && current !== source) {
    source = current;
    PANEL_TRANSLATE_TEXT_SOURCES.set(node, source);
  }
  const next = state.language === 'en' ? translatePanelText(source) : source;
  if (node.nodeValue !== next) node.nodeValue = next;
}

function applyPanelTranslations(root = document.body) {
  if (!root || !document.body) return;
  const base = root.nodeType === Node.ELEMENT_NODE ? root : document.body;
  if (base.closest && base.closest(PANEL_TRANSLATE_SKIP)) return;
  const walker = document.createTreeWalker(base, NodeFilter.SHOW_TEXT, {
    acceptNode(node) {
      const parent = node.parentElement;
      if (!parent || parent.closest(PANEL_TRANSLATE_SKIP)) return NodeFilter.FILTER_REJECT;
      return (node.nodeValue || '').trim() ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_REJECT;
    }
  });
  let node = walker.nextNode();
  while (node) {
    translatePanelNodeText(node);
    node = walker.nextNode();
  }
  const attrSelector = PANEL_TRANSLATABLE_ATTRS.map(attr => `[${attr}]`).join(',');
  base.querySelectorAll(attrSelector).forEach(el => PANEL_TRANSLATABLE_ATTRS.forEach(attr => {
    if (el.hasAttribute(attr)) translatePanelAttr(el, attr);
  }));
  document.documentElement.lang = state.language === 'en' ? 'en' : 'ru';
}

function queuePanelTranslations(root = document.body, force = false) {
  // Russian is the native panel language. Watching every DOM change there used
  // to trigger a whole-document translation scan after each render, even though
  // there was nothing to translate. A forced scan is kept for switching back
  // from English so original labels are restored correctly.
  if (state.language !== 'en' && !force) return;
  if (force) panelTranslationForceQueued = true;
  if (panelTranslationQueued) return;
  panelTranslationQueued = true;
  window.requestAnimationFrame(() => {
    panelTranslationQueued = false;
    const shouldApply = state.language === 'en' || panelTranslationForceQueued;
    panelTranslationForceQueued = false;
    if (!shouldApply) return;
    applyPanelTranslations(document.body || root);
  });
}

function stopPanelTranslationObserver() {
  if (!panelTranslationObserver) return;
  panelTranslationObserver.disconnect();
  panelTranslationObserver = null;
}

function startPanelTranslationObserver() {
  if (state.language !== 'en' || panelTranslationObserver || !window.MutationObserver || !document.body) return;
  panelTranslationObserver = new MutationObserver(mutations => {
    for (const mutation of mutations) {
      if (mutation.type === 'childList' || mutation.type === 'characterData' || mutation.type === 'attributes') {
        queuePanelTranslations(mutation.target && mutation.target.nodeType === Node.ELEMENT_NODE ? mutation.target : document.body);
        return;
      }
    }
  });
  panelTranslationObserver.observe(document.body, {
    subtree: true,
    childList: true,
    characterData: true,
    attributes: true,
    attributeFilter: PANEL_TRANSLATABLE_ATTRS
  });
}

function t(key, fallback) {
  const lang = panelI18n[state.language] ? state.language : 'ru';
  const parts = String(key || '').split('.');
  let value = panelI18n[lang];
  for (const part of parts) value = value && value[part];
  if (value == null && lang !== 'ru') {
    value = panelI18n.ru;
    for (const part of parts) value = value && value[part];
  }
  return value == null ? (fallback || key) : value;
}

function translatePanelResultBlocks() {
  document.querySelectorAll('.result').forEach(el => {
    const current = el.textContent || '';
    if (!current.trim()) return;
    const trimmed = current.trim();
    if (/^[{\[]/.test(trimmed)) return;
    let source = el.dataset ? el.dataset.i18nResultSource : '';
    if (!source || (state.language === 'ru' && current !== source)) {
      source = current;
      if (el.dataset) el.dataset.i18nResultSource = source;
    }
    if (state.language === 'en' && source && current !== translatePanelText(source)) {
      el.textContent = source.split('\n').map(line => translatePanelText(line)).join('\n');
    } else if (state.language === 'ru' && source && current !== source) {
      el.textContent = source;
    }
  });
}

function setPanelLanguage(language) {
  state.language = panelI18n[language] ? language : 'ru';
  localStorage.setItem('nedjin.language.v1', state.language);
  document.documentElement.lang = state.language;
  if (els.panelLanguage) els.panelLanguage.value = state.language;
  const title = t(`pages.${state.page}`, state.page);
  if (els.pageTitle) els.pageTitle.textContent = title;
  if (els.crumb) els.crumb.textContent = `${t('platform', 'Платформа')} / ${title}`;
  translatePanelResultBlocks();
  if (state.language === 'en') startPanelTranslationObserver();
  else stopPanelTranslationObserver();
  queuePanelTranslations(document.body, true);
}

function parseMaybeJson(value) {
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text) return value;
  if (!/^[{\[]/.test(text) && !/^"\s*[{\[]/.test(text)) return value;
  try {
    const parsed = JSON.parse(text);
    return typeof parsed === 'string' ? parseMaybeJson(parsed) : parsed;
  } catch (_) {
    return value;
  }
}

function findJsonObjectEnd(text, start) {
  let depth = 0;
  let inString = false;
  let escaped = false;
  for (let i = start; i < text.length; i += 1) {
    const ch = text[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch === '\\') {
        escaped = true;
      } else if (ch === '"') {
        inString = false;
      }
      continue;
    }
    if (ch === '"') {
      inString = true;
    } else if (ch === '{') {
      depth += 1;
    } else if (ch === '}') {
      depth -= 1;
      if (depth === 0) return i;
      if (depth < 0) return -1;
    }
  }
  return -1;
}

function salvageJsonObjects(text) {
  const source = String(text || '');
  const dataStart = source.indexOf('"data":[');
  let cursor = dataStart >= 0 ? dataStart + 8 : 0;
  const out = [];
  while (cursor < source.length) {
    const start = source.indexOf('{', cursor);
    if (start < 0) break;
    const end = findJsonObjectEnd(source, start);
    if (end < 0) break;
    const chunk = source.slice(start, end + 1);
    try {
      const parsed = JSON.parse(chunk);
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) out.push(parsed);
      cursor = end + 1;
    } catch (_) {
      cursor = start + 1;
    }
  }
  return out;
}

function unwrapApiEnvelope(data) {
  const parsed = parseMaybeJson(data);
  if (parsed && typeof parsed === 'object') {
    if (Object.prototype.hasOwnProperty.call(parsed, 'data')) return parseMaybeJson(parsed.data);
    if (Object.prototype.hasOwnProperty.call(parsed, 'payload')) return parseMaybeJson(parsed.payload);
  }
  return parsed;
}

function arrayFromPayload(data, keys = []) {
  const parsed = unwrapApiEnvelope(data);
  if (Array.isArray(parsed)) return parsed;
  if (!parsed || typeof parsed !== 'object') return [];
  const candidates = keys.concat(['rows', 'items', 'events', 'recent', 'entries', 'data', 'payload']);
  for (const key of candidates) {
    const value = parseMaybeJson(parsed[key]);
    if (Array.isArray(value)) return value;
    if (value && typeof value === 'object') {
      const nested = arrayFromPayload(value, keys);
      if (nested.length) return nested;
    }
  }
  return [];
}

const scumChatTextEncoder = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;

function scumChatSafeMessage(value, maxBytes = 220) {
  const normalized = String(value || '')
    .replace(/[\r\n\t]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  if (!scumChatTextEncoder) return normalized.slice(0, maxBytes).trim();
  let output = '';
  let usedBytes = 0;
  for (const ch of normalized) {
    if (ch === '\uFFFD') continue;
    const bytes = scumChatTextEncoder.encode(ch).length;
    if (usedBytes + bytes > maxBytes) break;
    output += ch;
    usedBytes += bytes;
  }
  return output.trim();
}

function api(path, opts = {}) {
  if (!state.apiKey && path !== '/api/health') {
    return Promise.reject(new Error('Ключ доступа не задан.'));
  }
  const headers = Object.assign({ 'X-API-KEY': state.apiKey }, opts.headers || {});
  if (opts.body && !(opts.body instanceof FormData)) headers['Content-Type'] = 'application/json';
  const base = normalizeApiBase(state.apiBase);
  const url = base && /^https?:\/\//i.test(base) ? `${base}${path}` : path;
  return fetch(url, Object.assign({}, opts, { headers, cache: 'no-store' }))
    .then(async res => {
      const text = await res.text();
      const data = text ? parseMaybeJson(text) : {};
      if (typeof data === 'string' && path.includes('/api/kills')) {
        const salvaged = salvageJsonObjects(text);
        if (salvaged.length) return salvaged;
      }
      if (!res.ok || data.ok === false) throw new Error(data.error || `${res.status} ${res.statusText}`);
      return unwrapApiEnvelope(data);
    });
}

function detectDiagnosticsProblem(status, bridgeOnline) {
  if (!status || typeof status !== 'object') return null;
  const diagnostics = status.diagnostics || {};
  const detected = diagnostics.problem || diagnostics.crash || null;
  if (detected && typeof detected === 'object' && (detected.hasProblem || detected.active || detected.severity === 'fatal')) {
    return detected;
  }
  const bridge = status.bridge || {};
  const database = status.database || {};
  const logs = status.logs || {};
  const candidates = [
    bridge.message,
    database.message,
    logs.message,
    status.error,
    status.message
  ].map(value => String(value || '')).filter(Boolean);
  const text = candidates.join(' | ');
  if (!bridgeOnline) return text || 'Bridge is offline or heartbeat is stale.';
  if (database.online === false) return text || 'SCUM database is not readable.';
  if (logs.online === false) return text || 'SCUM server log is not readable.';
  if (/(fatal|exception|crash|lua|ue4ss|failed|error|unreadable|stale|timeout)/i.test(text)) return text;
  return null;
}

function diagnosticProblemTitle(problem) {
  if (problem && typeof problem === 'object') {
    return problem.title || problem.Title || translatePanelText('Обнаружена ошибка SCUM NeDjin');
  }
  return translatePanelText('Обнаружена ошибка SCUM NeDjin');
}

function diagnosticProblemText(problem) {
  if (problem && typeof problem === 'object') {
    const message = problem.message || problem.Message || '';
    const source = problem.source || problem.Source || '';
    const at = problem.detectedAtUtc || problem.DetectedAtUtc || '';
    const parts = [message, source ? `Источник: ${source}.` : '', at ? `UTC: ${at}.` : ''].filter(Boolean);
    return parts.join(' ');
  }
  return String(problem || '').trim();
}

function updateDiagnosticSupport(status, problem) {
  state.lastStatus = status || null;
  state.lastDiagnosticsProblem = problem || null;
  if (!els.diagnosticSupport) return;
  const visible = !!problem;
  els.diagnosticSupport.classList.toggle('hidden', !visible);
  if (!visible) return;
  if (els.diagnosticSupportTitle) {
    els.diagnosticSupportTitle.textContent = diagnosticProblemTitle(problem);
  }
  if (els.diagnosticSupportMessage) {
    const note = diagnosticProblemText(problem);
    const actionText = translatePanelText('Скачайте диагностический пакет, откройте тикет в Discord и прикрепите файл вручную.');
    els.diagnosticSupportMessage.textContent = note
      ? `${note} ${actionText}`
      : actionText;
  }
}

function diagnosticsSupportMessage() {
  const problem = diagnosticProblemText(state.lastDiagnosticsProblem) || 'SCUM NeDjin diagnostics package';
  return [
    'SCUM NeDjin diagnostic report',
    `Problem: ${problem}`,
    `Panel: ${state.apiBase || window.location.origin}`,
    'Open a Discord ticket: https://discord.gg/kQwBDdzxEH',
    'Attach the downloaded diagnostics file.'
  ].join('\n');
}

function diagnosticsArchiveUrl() {
  const base = normalizeApiBase(state.apiBase);
  return base && /^https?:\/\//i.test(base) ? `${base}/api/diagnostics/archive` : '/api/diagnostics/archive';
}

function responseFileName(response, fallback) {
  const disposition = response && response.headers ? response.headers.get('content-disposition') || '' : '';
  const utf = disposition.match(/filename\*=UTF-8''([^;]+)/i);
  if (utf) return decodeURIComponent(utf[1].replace(/"/g, ''));
  const plain = disposition.match(/filename="?([^";]+)"?/i);
  return plain ? plain[1] : fallback;
}

async function downloadDiagnosticsPackage() {
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const headers = state.apiKey ? { 'X-API-KEY': state.apiKey } : {};
  const response = await fetch(diagnosticsArchiveUrl(), { headers, cache: 'no-store' });
  if (!response.ok) {
    const text = await response.text().catch(() => '');
    throw new Error(text || `${response.status} ${response.statusText}`);
  }
  const blob = await response.blob();
  const fileName = responseFileName(response, `scum-nedjin-diagnostics-${stamp}.zip`);
  downloadBlob(blob, fileName);
  if (navigator.clipboard && navigator.clipboard.writeText) {
    await navigator.clipboard.writeText(diagnosticsSupportMessage()).catch(() => {});
  }
  toast('Диагностический архив скачан. Краткий текст для Discord скопирован, если браузер разрешил доступ к буферу.');
}

function downloadBlob(blob, fileName) {
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = fileName || `scum-nedjin-diagnostics-${stamp}.zip`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

function resultTextValue(value) {
  if (value == null) return '';
  if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') return String(value);
  return '';
}

function firstResultText(obj, keys) {
  if (!obj || typeof obj !== 'object') return '';
  for (const key of keys) {
    const value = resultTextValue(obj[key]);
    if (value) return value;
  }
  return '';
}

function humanTransportName(value) {
  const text = String(value || '').toLowerCase();
  if (!text) return '';
  if (text.includes('announce')) return 'объявление сервера';
  if (text.includes('queue') || text.includes('admin') || text.includes('exec')) return 'игровая очередь';
  if (text.includes('rcon') || text.includes('nedjin')) return 'командный канал';
  return '';
}

function summarizeQueuedResult(queued) {
  if (!queued || typeof queued !== 'object') return [];
  const lines = [];
  const requested = Number(queued.requested ?? queued.total ?? 0);
  const queuedCount = Number(queued.queued ?? queued.count ?? 0);
  const chunks = Number(queued.chunks ?? 0);
  if (Number.isFinite(queuedCount) && queuedCount > 0) {
    lines.push(`В очереди: ${queuedCount}${Number.isFinite(requested) && requested > 0 ? ` из ${requested}` : ''}.`);
  } else if (Number.isFinite(requested) && requested > 0 && queued.ok !== false) {
    lines.push(`Отправлено: ${requested}.`);
  }
  if (Number.isFinite(chunks) && chunks > 1) lines.push(`Пакетов: ${chunks}.`);
  const failed = Array.isArray(queued.failed) ? queued.failed : [];
  if (failed.length) lines.push(`Не получилось: ${failed.slice(0, 3).map(item => String(item)).join('; ')}${failed.length > 3 ? '...' : ''}`);
  return lines;
}

function formatPanelResult(value) {
  const parsed = parseMaybeJson(value);
  if (typeof parsed === 'string') return parsed;
  if (Array.isArray(parsed)) return parsed.length ? `Получено записей: ${parsed.length}.` : 'Записей нет.';
  if (!parsed || typeof parsed !== 'object') return resultTextValue(parsed) || '';

  const lines = [];
  const ok = parsed.ok ?? parsed.success;
  if (ok === true) lines.push('Готово.');
  if (ok === false) lines.push('Не выполнено.');

  const primary = firstResultText(parsed, ['message', 'error', 'result', 'statusText', 'status']);
  if (primary && !lines.includes(primary)) lines.push(primary);

  const nested = parsed.data && typeof parsed.data === 'object' ? parsed.data : {};
  const nestedMessage = firstResultText(nested, ['message', 'error', 'result', 'statusText', 'status']);
  if (nestedMessage && !lines.includes(nestedMessage)) lines.push(nestedMessage);

  const transport = humanTransportName(parsed.transport || nested.transport || parsed.source || nested.source);
  const command = firstResultText(parsed, ['command', 'adminCommand', 'commandText']) || firstResultText(nested, ['command', 'adminCommand', 'commandText']);
  if (command) lines.push(transport ? `Команда отправлена через ${transport}.` : 'Команда отправлена.');

  for (const line of summarizeQueuedResult(parsed.queued || nested.queued)) {
    if (!lines.includes(line)) lines.push(line);
  }

  const failed = Array.isArray(parsed.failed) ? parsed.failed : (Array.isArray(nested.failed) ? nested.failed : []);
  if (failed.length) lines.push(`Не получилось: ${failed.slice(0, 3).map(item => String(item)).join('; ')}${failed.length > 3 ? '...' : ''}`);

  if (!lines.length && Object.keys(parsed).length) return 'Операция завершена.';
  return lines.join('\n');
}

function showResult(el, value) {
  if (!el) return;
  if (typeof value === 'string') {
    if (el.dataset) el.dataset.i18nResultSource = value;
    el.textContent = translatePanelText(value);
    return;
  }
  if (el.classList && el.classList.contains('profile-result') && value && typeof value === 'object') {
    const primary = value.message || value.error || value.result;
    if (primary) {
      if (el.dataset) el.dataset.i18nResultSource = String(primary);
      el.textContent = translatePanelText(String(primary));
      return;
    }
  }
  const sourceText = formatPanelResult(value);
  if (el.dataset) el.dataset.i18nResultSource = sourceText;
  el.textContent = sourceText.split('\n').map(line => translatePanelText(line)).join('\n');
}

async function showActionResult(el, action) {
  try {
    showResult(el, 'Выполняется...');
    showResult(el, await action());
  } catch (err) {
    showResult(el, { ok: false, error: err && err.message ? err.message : String(err) });
  }
}

function sleep(ms) {
  return new Promise(resolve => window.setTimeout(resolve, ms));
}

function fmtTime(value) {
  if (!value) return '';
  const dt = new Date(value);
  return Number.isNaN(dt.getTime()) ? String(value) : dt.toLocaleString();
}

function unwrapCatalogData(data, key) {
  if (Array.isArray(data)) return data;
  if (data && Array.isArray(data[key])) return data[key];
  if (data && data.data && Array.isArray(data.data[key])) return data.data[key];
  if (data && data.payload && Array.isArray(data.payload[key])) return data.payload[key];
  return [];
}

async function fetchJsonAsset(path) {
  return fetch(path, { cache: 'no-store' }).then(res => res.ok ? res.json() : null).catch(() => null);
}

async function fetchCachedJsonAsset(path) {
  const separator = String(path).includes('?') ? '&' : '?';
  return fetch(`${path}${separator}v=${encodeURIComponent(STATIC_ASSET_VERSION)}`, { cache: 'force-cache' })
    .then(res => res.ok ? res.json() : null)
    .catch(() => null);
}

function normalizeCatalogEntry(entry, kind) {
  const id = kind === 'vehicle'
    ? (entry.vehicleId || entry.assetName || entry.AssetName || entry.id || '')
    : (entry.itemId || entry.runtimeId || entry.id || '');
  const name = entry.name || entry.displayName || entry.DisplayName || id;
  return Object.assign({}, entry, {
    itemId: kind === 'item' ? id : undefined,
    vehicleId: kind === 'vehicle' ? id : undefined,
    name,
    category: entry.category || entry.Category || (kind === 'vehicle' ? 'Транспорт' : 'Другое')
  });
}

function normalizeLookup(value) {
  return String(value || '')
    .trim()
    .replace(/^.*[:/\\]/, '')
    .replace(/^.*\./, '')
    .replace(/_C$/i, '')
    .replace(/_ES$/i, '')
    .replace(/[^a-z0-9]+/gi, '')
    .toLowerCase();
}

function iconLookupKeys(value) {
  const raw = String(value || '').trim();
  if (!raw) return [];
  const cleaned = cleanAssetId(raw);
  return Array.from(new Set([
    raw,
    raw.toLowerCase(),
    cleaned,
    cleaned.toLowerCase(),
    normalizeLookup(raw),
    normalizeLookup(cleaned)
  ].filter(Boolean)));
}

function registerIconAlias(alias, url, force = false) {
  if (!alias || !url) return;
  for (const variant of iconLookupKeys(alias)) {
    if (force || !state.itemIconLookup.has(variant)) state.itemIconLookup.set(variant, url);
  }
}

function iconRecordUrl(record) {
  if (!record) return '';
  if (typeof record === 'string') return record.trim();
  if (typeof record.url === 'string') return record.url.trim();
  if (typeof record.src === 'string') return record.src.trim();
  if (typeof record.path === 'string') return record.path.trim();
  if (typeof record.local === 'string') return record.local.trim();
  if (typeof record.image === 'string') return record.image.trim();
  if (typeof record.thumbnail === 'string') return record.thumbnail.trim();
  return '';
}

function iconRecordAliases(key, record) {
  const aliases = [key];
  if (record && typeof record === 'object') {
    const rawAliases = Array.isArray(record.aliases) ? record.aliases : [];
    aliases.push(
      record.itemId,
      record.ItemId,
      record.id,
      record.name,
      record.displayName,
      record.icon,
      record.iconName,
      ...rawAliases
    );
  }
  return Array.from(new Set(aliases.filter(Boolean)));
}

function registerItemIconOverrides(overrides) {
  const data = overrides && typeof overrides === 'object' ? overrides : {};
  const icons = data.icons || data.items || data.overrides || {};
  const aliases = data.aliases || {};
  state.itemIconOverrides = data;
  if (icons && typeof icons === 'object') {
    for (const [key, record] of Object.entries(icons)) {
      const url = iconRecordUrl(record);
      if (!url) continue;
      for (const alias of iconRecordAliases(key, record)) registerIconAlias(alias, url, true);
    }
  }
  if (aliases && typeof aliases === 'object') {
    for (const [alias, target] of Object.entries(aliases)) {
      const recordUrl = iconRecordUrl(target);
      const targetUrl = recordUrl || getItemIconUrl(target) || iconRecordUrl(icons[target]);
      if (targetUrl) registerIconAlias(alias, targetUrl, true);
    }
  }
}

function buildItemIconLookup(manifest) {
  const icons = manifest && manifest.icons && typeof manifest.icons === 'object' ? manifest.icons : {};
  const aliases = manifest && manifest.aliases && typeof manifest.aliases === 'object' ? manifest.aliases : {};
  state.itemIcons = icons;
  state.itemIconLookup = new Map();
  state.generatedIconCache = new Map();
  for (const [key, url] of Object.entries(icons)) {
    if (!url) continue;
    registerIconAlias(key, url);
  }
  for (const [alias, target] of Object.entries(aliases)) {
    const url = getItemIconUrl(target);
    if (url) registerIconAlias(alias, url);
  }
}

function buildCatalogIconAliases(entries = []) {
  for (const entry of entries || []) {
    const probes = [
      entry.itemId,
      entry.vehicleId,
      entry.assetName,
      entry.AssetName,
      entry.id,
      entry.alias,
      entry.Alias,
      entry.displayName,
      entry.DisplayName,
      entry.name,
      entry.runtimeId,
      entry.classPath,
      entry.assetPath,
      entry.itemClass,
      entry.icon,
      entry.Icon,
      entry.iconName,
      entry.image,
      entry.thumbnail
    ];
    let url = '';
    for (const probe of probes) {
      url = getItemIconUrl(probe);
      if (url) break;
    }
    if (!url) continue;
    for (const probe of probes) registerIconAlias(probe, url);
  }

  const fixedAliases = {
    Apple_2: 'Apple',
    CannedGoulash: 'ICO_Canned_Goulash',
    Emergency_bandage_Big: 'ICO_Bandage_pack_Vicinity',
    MRE_Cheeseburger: 'ICO_Cheeseburger',
    MRE: 'ICO_Cheeseburger',
    Tactical_Jacket_01_03: 'ICO_Jacket_03_03',
    MilitaryPants_03: 'ICO_Trousers_02_04',
    Military_Backpack_01_03: 'ICO_Assault_Backpack_02',
    Military_Helmet_01_03: 'ICO_Military_Helmet_02_03',
    Bulletproof_Vest_01: 'ICO_Bulletproof_Vest_01',
    CombatBoots: 'ICO_Boots',
    Weapon_AK47: 'ICO_AK47_Vicinity',
    Cal_7_62x39mm: 'ICO_Cal_7_62x39mm_Pile',
    Magazine_RPK: 'ICO_RPK_Magazine_Empty',
    Water_05l: 'PETBottle01',
    Water_05L: 'PETBottle01',
    ci_water_bottle: 'PETBottle01',
    Weapon_BlackHawk_Crossbow: 'Weapon_Improvised_AutoCrossbow',
    '1H_KitchenKnife': '1H_KitchenKnife_02'
  };
  for (const [alias, target] of Object.entries(fixedAliases)) {
    const url = getItemIconUrl(target);
    if (url) registerIconAlias(alias, url);
  }
}

function registerVehicleIconAliases() {
  const vehicleIcons = {
    BPC_Rager: 'ICO_Rager',
    rager: 'ICO_Rager',
    Rager: 'ICO_Rager',
    BPC_WolfsWagen: 'ICO_WolfsWagen',
    WolfsWagen: 'ICO_WolfsWagen',
    Wolfswagen: 'ICO_WolfsWagen',
    wolfswaggen: 'ICO_WolfsWagen',
    BPC_Laika: 'ICO_Laika',
    Laika: 'ICO_Laika',
    BPC_RIS: 'ICO_RIS',
    RIS: 'ICO_RIS',
    'RIS Quad': 'ICO_RIS',
    BPC_Dirtbike: 'ICO_Motorcycle_01_A',
    Dirtbike: 'ICO_Motorcycle_01_A',
    'Dirt Bike': 'ICO_Motorcycle_01_A',
    BPC_Cruiser: 'ICO_Cruiser',
    Cruiser: 'ICO_Cruiser',
    BPC_Sportbike: 'ICO_Motorcycle_03',
    Sportbike: 'ICO_Motorcycle_03',
    BPC_SidecarBike: 'ICO_SidecarBike',
    SidecarBike: 'ICO_SidecarBike',
    'Sidecar Bike': 'ICO_SidecarBike',
    BPC_CityBike: 'ICO_Bicycle_01_A',
    CityBike: 'ICO_Bicycle_01_A',
    'City Bike': 'ICO_Bicycle_01_A',
    BPC_MountainBike: 'ICO_Bicycle_02_A',
    MountainBike: 'ICO_Bicycle_02_A',
    'Mountain Bike': 'ICO_Bicycle_02_A',
    BPC_Barba: 'ICO_MotorBoat_01_A',
    Barba: 'ICO_MotorBoat_01_A',
    'Wooden Motorboat': 'ICO_MotorBoat_01_A',
    BP_Motorboat_02: 'ICO_Motorboat_02',
    Motorboat: 'ICO_Motorboat_02',
    'Rubber Motorboat': 'ICO_Motorboat_02',
    BPC_SUP: 'ICO_SUP_Vicinity',
    SUP: 'ICO_SUP_Vicinity',
    BP_Improvised_Raft_Small: 'ICO_ImproSmallRaft',
    SmallRaft: 'ICO_ImproSmallRaft',
    'Small Improvised Raft': 'ICO_ImproSmallRaft',
    BPC_BigRaft: 'ICO_ImproRaftBig',
    BigRaft: 'ICO_ImproRaftBig',
    'Big Improvised Raft': 'ICO_ImproRaftBig',
    BPC_Dinghy: 'ICO_Motorboat_02',
    Dinghy: 'ICO_Motorboat_02',
    BPC_Kinglet_Duster: 'ICO_Kinglet_Duster_A',
    KingletDuster: 'ICO_Kinglet_Duster_A',
    'Kinglet Duster': 'ICO_Kinglet_Duster_A',
    BPC_Kinglet_Scout: 'ICO_Kinglet_Duster_A',
    KingletScout: 'ICO_Kinglet_Duster_A',
    'Kinglet Scout': 'ICO_Kinglet_Duster_A',
    BPC_Kinglet_Mariner: 'ICO_Kinglet_Mariner',
    KingletMariner: 'ICO_Kinglet_Mariner',
    'Kinglet Mariner': 'ICO_Kinglet_Mariner',
    BP_WheelBarrow_02: 'ICO_ImproWheelbarrow',
    WheelBarrow: 'ICO_ImproWheelbarrow',
    'Improvised Wheelbarrow': 'ICO_ImproWheelbarrow',
    BP_WheelBarrow_Metal: 'ICO_Wheelbarrow',
    MetalWheelBarrow: 'ICO_Wheelbarrow',
    'Metal Wheelbarrow': 'ICO_Wheelbarrow',
    BPC_Tractor: 'ICO_Tractor_01_A',
    Tractor: 'ICO_Tractor_01_A'
  };
  for (const [alias, target] of Object.entries(vehicleIcons)) {
    const url = getItemIconUrl(target);
    if (url) registerIconAlias(alias, url);
  }
}

function refreshCatalogIconAliases() {
  registerVehicleIconAliases();
  buildCatalogIconAliases(state.items);
  buildCatalogIconAliases(state.vehicles);
}

function rerenderIconDependentSurfaces() {
  renderCharacterPackCatalog();
  renderModuleCatalog();
  renderWargmItems();
  if (state.moduleDraft && els.moduleFields) renderModuleFields(state.moduleDraft);
}

async function loadItemIconAssets(options = {}) {
  if (state.itemIconAssetsLoaded) return true;
  if (state.itemIconAssetsPromise) return state.itemIconAssetsPromise;
  const rerender = Boolean(options.rerender);
  state.itemIconAssetsLoading = true;
  state.itemIconAssetsPromise = Promise.all([
    fetchCachedJsonAsset('/item-icons-manifest.json'),
    fetchCachedJsonAsset('/item-icon-overrides.json')
  ]).then(([manifest, overrides]) => {
    buildItemIconLookup(manifest || {});
    registerItemIconOverrides(overrides || {});
    refreshCatalogIconAliases();
    state.itemIconAssetsLoaded = true;
    state.generatedIconCache = new Map();
    if (rerender) rerenderIconDependentSurfaces();
    return true;
  }).catch(() => false).finally(() => {
    state.itemIconAssetsLoading = false;
    state.itemIconAssetsPromise = null;
  });
  return state.itemIconAssetsPromise;
}

function getItemIconUrl(value) {
  for (const key of iconLookupKeys(value)) {
    const url = state.itemIconLookup.get(key);
    if (url) return url;
  }
  return '';
}

function getCatalogIconUrl(entry, kind) {
  const probes = kind === 'vehicle'
    ? [
      entry.vehicleId,
      entry.assetName,
      entry.AssetName,
      entry.id,
      entry.alias,
      entry.Alias,
      entry.displayName,
      entry.DisplayName,
      entry.name,
      entry.icon,
      entry.Icon,
      entry.iconName,
      entry.image,
      entry.thumbnail
    ]
    : [
      entry.itemId,
      entry.id,
      entry.runtimeId,
      entry.classPath,
      entry.assetPath,
      entry.itemClass,
      entry.icon,
      entry.Icon,
      entry.iconName,
      entry.image,
      entry.thumbnail
    ];
  for (const probe of probes) {
    const url = getItemIconUrl(probe);
    if (url) return url;
  }
  return '';
}

function generatedCatalogIconUrl(entry, kind) {
  const value = catalogValue(entry || {}, kind) || entry.name || entry.displayName || kind || 'item';
  const category = String(entry.category || kind || 'item').toLowerCase();
  const cacheKey = `${kind}:${category}:${value}`;
  if (state.generatedIconCache.has(cacheKey)) return state.generatedIconCache.get(cacheKey);
  const symbol = categorySymbol(entry || {}, kind);
  const palette = kind === 'vehicle'
    ? ['#312412', '#d99a2b']
    : /weapon|rifle|pistol|shotgun|bow|melee/.test(category)
      ? ['#1c2630', '#ee5b43']
      : /ammo|magazine|bullet|cartridge/.test(category)
        ? ['#222313', '#f0b94a']
        : /food|drink|water|meat|fruit|vegetable/.test(category)
          ? ['#15251d', '#2bb673']
          : /medical|bandage|pill|syringe|antibiotic/.test(category)
            ? ['#20191d', '#e35d6a']
            : /clothing|jacket|pants|boots|helmet|vest/.test(category)
              ? ['#17202b', '#72a7d8']
              : ['#17202b', '#8aa0b7'];
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">
    <defs><linearGradient id="g" x1="0" x2="1" y1="0" y2="1"><stop stop-color="${palette[0]}"/><stop offset="1" stop-color="#070b10"/></linearGradient></defs>
    <rect width="96" height="96" rx="16" fill="url(#g)"/>
    <rect x="10" y="10" width="76" height="76" rx="12" fill="none" stroke="${palette[1]}" stroke-opacity=".42" stroke-width="2"/>
    <circle cx="73" cy="24" r="8" fill="${palette[1]}" fill-opacity=".22"/>
    <text x="48" y="56" text-anchor="middle" font-family="Inter,Segoe UI,Arial,sans-serif" font-size="23" font-weight="800" fill="#f3f6fb">${symbol}</text>
  </svg>`;
  const url = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
  state.generatedIconCache.set(cacheKey, url);
  return url;
}

function cleanAssetId(value) {
  const raw = String(value || '').trim();
  if (!raw) return '';
  return raw
    .replace(/^.*[:/\\]/, '')
    .replace(/^.*\./, '')
    .replace(/\s+S\[.*$/i, '')
    .replace(/\s+C\[.*$/i, '')
    .replace(/\s*\[[^\]]+\].*$/i, '')
    .replace(/_C_\d+$/i, '')
    .replace(/_C$/i, '')
    .replace(/_ES$/i, '');
}

const CP1251_DECODE_TABLE = '\u0402\u0403\u201A\u0453\u201E\u2026\u2020\u2021\u20AC\u2030\u0409\u2039\u040A\u040C\u040B\u040F\u0452\u2018\u2019\u201C\u201D\u2022\u2013\u2014\uFFFD\u2122\u0459\u203A\u045A\u045C\u045B\u045F\u00A0\u040E\u045E\u0408\u00A4\u0490\u00A6\u00A7\u0401\u00A9\u0404\u00AB\u00AC\u00AD\u00AE\u0407\u00B0\u00B1\u0406\u0456\u0491\u00B5\u00B6\u00B7\u0451\u2116\u0454\u00BB\u0458\u0405\u0455\u0457\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041A\u041B\u041C\u041D\u041E\u041F\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042A\u042B\u042C\u042D\u042E\u042F\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043A\u043B\u043C\u043D\u043E\u043F\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044A\u044B\u044C\u044D\u044E\u044F';
const CP1251_ENCODE = (() => {
  const map = new Map();
  for (let i = 0; i < 128; i += 1) map.set(String.fromCharCode(i), i);
  for (let i = 0; i < CP1251_DECODE_TABLE.length; i += 1) {
    const ch = CP1251_DECODE_TABLE[i];
    if (ch !== '\uFFFD') map.set(ch, i + 128);
  }
  return map;
})();

function mojibakeScore(value) {
  const text = String(value || '');
  let score = 0;
  const hits = text.match(/[РСÐÑ][\u0080-\u04ff]|пїЅ|\?{2,}/g);
  if (hits) score += hits.length * 8;
  if (/[РС][А-Яа-яЁёІЇЄЎЃЌЉЊЋЏЂЈЌЋ]/.test(text)) score += 8;
  if (/[А-Яа-яЁё]/.test(text)) score -= 2;
  return score;
}

function repairMojibakeText(value) {
  const text = String(value || '');
  if (!text || !/[РСр][\u0400-\u04ff]|пїЅ|\?{2,}/.test(text)) return text;
  const bytes = [];
  for (const ch of text) {
    if (!CP1251_ENCODE.has(ch)) return text;
    bytes.push(CP1251_ENCODE.get(ch));
  }
  let repaired = '';
  try {
    repaired = new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(bytes));
  } catch (_) {
    return text;
  }
  return mojibakeScore(repaired) < mojibakeScore(text) ? repaired : text;
}

function findCatalogMatch(value, kind = 'item') {
  const list = kind === 'vehicle' ? state.vehicles : state.items;
  const needle = normalizeLookup(value);
  if (!needle) return null;
  return (list || []).find(entry => {
    const id = kind === 'vehicle' ? (entry.vehicleId || entry.assetName || entry.AssetName || entry.id) : (entry.itemId || entry.runtimeId || entry.id);
    return [id, entry.name, entry.displayName, entry.runtimeId, entry.classPath]
      .some(candidate => normalizeLookup(candidate) === needle);
  }) || null;
}

function englishNameFromAssetId(value) {
  const clean = cleanAssetId(value);
  if (!clean) return '';
  return clean
    .replace(/^BPC?_?/i, '')
    .replace(/_/g, ' ')
    .replace(/([a-z])([A-Z])/g, '$1 $2')
    .replace(/\bAmmobox\b/gi, 'Ammo Box')
    .replace(/\bBirdShot\b/gi, 'Bird Shot')
    .replace(/\bBuckshot\b/gi, 'Buckshot')
    .replace(/\bSlug\b/gi, 'Slug')
    .replace(/\s+/g, ' ')
    .trim();
}

function catalogDisplayName(entry, kind = 'item') {
  const value = catalogValue(entry || {}, kind);
  const raw = String((entry && (entry.name || entry.displayName || entry.DisplayName)) || value || '').trim();
  if (state.language === 'en' && /[А-Яа-яЁё]/.test(raw)) {
    return englishNameFromAssetId(value) || raw;
  }
  return raw || value || '-';
}

function friendlyAssetName(value, kind = 'item') {
  const match = findCatalogMatch(value, kind);
  return match ? catalogDisplayName(match, kind) : (englishNameFromAssetId(value) || cleanAssetId(value) || value || '-');
}

function catalogValue(entry, kind) {
  return kind === 'vehicle'
    ? (entry.vehicleId || entry.assetName || entry.AssetName || entry.id || '')
    : (entry.itemId || entry.runtimeId || entry.id || '');
}

function catalogIdFor(entry, kind) {
  return catalogValue(entry || {}, kind);
}

function shortCode(value, fallback = 'IT') {
  const words = String(value || fallback).replace(/[_-]+/g, ' ').trim().split(/\s+/).filter(Boolean);
  const code = words.length > 1 ? words.slice(0, 2).map(word => word[0]).join('') : String(words[0] || fallback).slice(0, 3);
  return code.toUpperCase();
}

const configLabels = {
  Enabled: 'Включено',
  RequiredPermission: 'Требуемое право',
  IncludeBangAliases: 'Также принимать !команды',
  Help: 'Помощь',
  Hello: 'Проверка связи',
  Language: 'Язык',
  SetHome: 'Установить дом',
  Home: 'Телепорт домой',
  Homes: 'Список домов',
  DeleteHome: 'Удалить дом',
  PrivateMessage: 'Личное сообщение',
  Reply: 'Ответить',
  PrivateMessageHistory: 'История личных сообщений',
  SectorScan: 'Скан сектора',
  FastTravel: 'Быстрое перемещение',
  BaseLoot: 'Сбор лута',
  Rent: 'Аренда транспорта',
  MoneyTransfer: 'Перевод денег',
  ItemUpgrade: 'Апгрейд предметов',
  Quest: 'Квесты',
  Streak: 'Серия убийств',
  Bounty: 'Bounty',
  HunterTop: 'Топ охотников',
  InventoryDelete: 'Удаление предмета',
  DiscordTest: 'Discord тест',
  ArmoryBack: 'Возврат Armory',
  Editor: 'Редактор',
  CooldownHours: 'Кулдаун, ч',
  SuccessMessage: 'Сообщение при успехе',
  SerializeClaimsGlobally: 'Выдавать по одному игроку за раз',
  InterItemDelayMs: 'Пауза между предметами, мс',
  Items: 'Предметы',
  ItemId: 'ID предмета',
  Quantity: 'Количество',
  DefaultAttributes: 'Атрибуты по умолчанию',
  Strength: 'Сила',
  Constitution: 'Телосложение',
  Dexterity: 'Ловкость',
  Intelligence: 'Интеллект',
  SkillPresets: 'Пресеты навыков',
  Packs: 'Пакеты персонажа',
  Actions: 'Действия',
  Name: 'Название',
  Description: 'Описание',
  Level: 'Уровень',
  Experience: 'Опыт',
  TransferCooldownMinutes: 'Кулдаун перемещения, мин',
  RatePerMeter: 'Цена за метр',
  FixedFare: 'Фиксированная цена',
  TeleportDelaySeconds: 'Задержка телепорта, сек',
  TravelConfirmationSeconds: 'Время подтверждения, сек',
  CancelMoveDistance: 'Отмена при движении, см',
  SuccessArrivalDistance: 'Радиус успешного прибытия',
  Outposts: 'Маршруты',
  DisplayName: 'Название',
  CommandAlias: 'Команда',
  CenterZone: 'Центр зоны',
  ArrivalPoint: 'Точка прибытия',
  Price: 'Цена маршрута',
  MaxHomes: 'Лимит домов',
  DefaultMaxHomes: 'Домов обычному игроку',
  VipMaxHomes: 'Домов VIP-игроку',
  VipPermission: 'Право VIP',
  DefaultTier: 'Уровень VIP по умолчанию',
  DefaultDurationDays: 'Срок VIP по умолчанию, дней',
  ExtendExisting: 'Продлевать активный VIP',
  BasePermissions: 'Базовые VIP-права',
  TierPermissions: 'Права уровней VIP',
  Permissions: 'Права игрока',
  Features: 'VIP возможности',
  WelcomePack: 'VIP стартовый набор',
  DailyPack: 'VIP ежедневный набор',
  HomeSystem: 'VIP дома',
  SectorScan: 'VIP скан сектора',
  VehicleRental: 'VIP аренда транспорта',
  DiscountPercent: 'VIP скидка, %',
  Free: 'Бесплатно для VIP',
  SpawnCooldownSeconds: 'Кулдаун спавна VIP, сек',
  Wargm: 'VIP через Wargm',
  HomeCooldownSeconds: 'Кулдаун дома, сек',
  Vehicles: 'Транспорт',
  VipVehicles: 'VIP транспорт',
  Alias: 'Команда',
  AssetName: 'ID транспорта',
  PricePer10Minutes: 'Цена / 10 мин',
  InitialCharge: 'Стартовая цена',
  DefaultRentalMinutes: 'Время по умолчанию, мин',
  MinRentalMinutes: 'Минимальное время, мин',
  MaxRentalMinutes: 'Максимальное время, мин',
  DefaultMinutes: 'Время по умолчанию, мин',
  MinMinutes: 'Минимум минут',
  MaxMinutes: 'Максимум минут',
  ChargePenaltyOnMissingVehicle: 'Штрафовать за пропавший транспорт',
  DefaultMissingVehiclePenalty: 'Штраф по умолчанию',
  MissingVehiclePenalty: 'Штраф за продажу/потерю',
  WarningMinutesBeforeExpiry: 'Предупреждения до конца аренды, мин',
  ProjectId: 'ID проекта',
  ApiKey: 'Ключ доступа',
  WargmServerId: 'ID сервера Wargm',
  ShopId: 'ID магазина GameStores',
  StoreId: 'ID магазина GameStores',
  SecretKey: 'Secret key GameStores',
  ServerId: 'ID сервера GameStores',
  GameStoresServerId: 'ID сервера GameStores',
  PollIntervalSeconds: 'Интервал опроса, сек',
  DeliveryDelaySeconds: 'Задержка выдачи, сек',
  RetryDelaySeconds: 'Повтор через, сек',
  MaxDeliveryAttempts: 'Попыток выдачи',
  DeliveryFilter: 'Фильтр выдачи',
  RequireRecipientName: 'Требовать имя получателя',
  DeliverOnlyToOnlinePlayers: 'Выдавать только онлайн',
  AllowDirectItemIdDelivery: 'Разрешить item_id как SCUM ID',
  AnnounceBeforeDelivery: 'Сообщать перед выдачей',
  AnnouncementTemplate: 'Шаблон объявления',
  BroadcastDeliveries: 'Сообщать всем о выдаче',
  AllowUnsafeCommandDelivery: 'Разрешить командную выдачу',
  UnsafeCommandInterItemDelayMs: 'Пауза между предметами, мс',
  UnsafeCommandInterOperationCooldownSeconds: 'Пауза между операциями, сек',
  UnsafeCommandMaxItemsPerOperation: 'Макс. предметов за операцию',
  UnsafeCommandMaxQuantityPerItem: 'Макс. количество предмета',
  UseClaimInsteadOfSuccess: 'Использовать claim вместо success',
  Rules: 'Правила',
  MatchOfferId: 'ID предложения',
  MatchTitleContains: 'Название содержит',
  DeliveryMode: 'Режим выдачи',
  DeliveryLabel: 'Метка выдачи',
  VehicleAsset: 'ID транспорта',
  Amount: 'Сумма',
  SkillName: 'Навык',
  SkillLevel: 'Уровень навыка',
  SkillExperience: 'Опыт навыка',
  CommandTemplate: 'Шаблон команды',
  WorldEventClass: 'Класс world event',
  WorldEventClasses: 'Классы world event',
  ScheduleTimes: 'Время запуска (HH:mm)',
  TimesOfDay: 'Время запуска (HH:mm)',
  RunTimes: 'Время запуска (HH:mm)',
  At: 'Запуск в',
  IntervalMinutes: 'Интервал, мин',
  RunOnStartup: 'Запускать при старте',
  MaxRunsPerTick: 'Макс. задач за тик',
  PointGroup: 'Группа точек',
  PointCount: 'Точек за запуск',
  ItemSet: 'Набор предметов',
  MaxItemsPerRun: 'Макс. предметов за запуск',
  Points: 'Точки планировщика',
  ItemSets: 'Наборы предметов',
  ItemsText: 'Предметы набора',
  ScanCost: 'Цена скана',
  CooldownSeconds: 'Кулдаун, сек',
  IncludeSelf: 'Считать самого игрока',
  IncludePlayerNames: 'Показывать имена игроков',
  MaxListedNames: 'Макс. имён в ответе',
  EmptyMessage: 'Сообщение пустого сектора',
  NotEnoughMoneyMessage: 'Сообщение нехватки денег',
  CooldownMessage: 'Сообщение кулдауна',
  BroadcastThreshold: 'Порог объявления',
  HeroThreshold: 'Порог героя',
  MaxBoardEntries: 'Строк в таблице',
  LeaderKillRewardMode: 'Режим награды за лидера',
  LeaderKillRewardAmount: 'Сумма награды за лидера',
  LeaderKillRewardStartStreak: 'Серия для награды',
  LeaderKillRewardStepAmount: 'Рост награды за убийство',
  BroadcastLeaderKillReward: 'Объявлять награду за лидера',
  Prefix: 'Префикс сообщений',
  Source: 'Источник данных',
  UseRuntimeHooks: 'Использовать live-хуки',
  AnnounceSuicides: 'Показывать самоубийства',
  IncludeDistance: 'Показывать дистанцию',
  IncludeWeapon: 'Показывать оружие',
  FanoutToPlayers: 'Рассылать игрокам',
  TransportMode: 'Режим отправки',
  ServerLabel: 'Название сервера',
  GuildId: 'ID Discord-сервера',
  DefaultChannelId: 'Канал по умолчанию',
  PresenceChannelId: 'Канал входов/выходов',
  ChatChannelId: 'Канал чата',
  CombatChannelId: 'Канал боёв',
  SystemChannelId: 'Системный канал',
  BotToken: 'Токен Discord-бота',
  DefaultWebhookUrl: 'Webhook по умолчанию',
  PresenceWebhookUrl: 'Webhook входов/выходов',
  ChatWebhookUrl: 'Webhook чата',
  CombatWebhookUrl: 'Webhook боёв',
  SystemWebhookUrl: 'Webhook системы',
  Username: 'Имя бота',
  AvatarUrl: 'Аватар',
  NotifyOnLoad: 'Сообщать о загрузке',
  NotifyPlayerConnected: 'Игрок вошёл',
  NotifyPlayerDisconnected: 'Игрок вышел',
  NotifyPlayerRespawned: 'Игрок возродился',
  NotifyPlayerChat: 'Писать чат',
  NotifyPlayerKills: 'Писать убийства',
  NotifyPlayerDeaths: 'Писать смерти',
  IncludeSteamId: 'Показывать SteamID',
  IgnoreSlashCommands: 'Игнорировать slash-команды',
  IgnoredChatPrefixes: 'Игнорируемые префиксы чата',
  ForwardLocalChat: 'Пересылать локальный чат',
  ForwardGlobalChat: 'Пересылать глобальный чат',
  ForwardSquadChat: 'Пересылать чат отряда',
  ForwardAdminChat: 'Пересылать админ-чат',
  MinPostIntervalMs: 'Минимальная пауза, мс',
  MaxQueueLength: 'Макс. очередь',
  DuplicateWindowSeconds: 'Окно дублей, сек',
  UsePermission: 'Проверять право',
  AllowPermission: 'Право доступа',
  UseCooldown: 'Включить кулдаун',
  CooldownTimeSeconds: 'Кулдаун, сек',
  EnableLogging: 'Вести лог',
  EnableHistory: 'Хранить историю',
  BaseUrl: 'Базовый URL',
  Model: 'Модель'
};

const selectLabels = {
  Normal: 'Обычная валюта',
  Currency: 'Обычная валюта',
  SpawnItem: 'Предмет',
  Vehicle: 'Транспорт',
  CargoDropPlayer: 'Cargo drop',
  Vip: 'VIP',
  Money: 'Баланс',
  Gold: 'Золото',
  Fame: 'Очки славы',
  Attributes: 'Атрибуты',
  Skill: 'Навык',
  AllSkills: 'Все навыки',
  PlayerCommand: 'Команда игроку',
  ServerCommand: 'Команда серверу'
};

const FULL_SCUM_SKILLS = [
  { skill: 'Resistance', name: 'Resistance', category: 'Physical' },
  { skill: 'Brawling', name: 'Brawling', category: 'Combat' },
  { skill: 'Awareness', name: 'Awareness', category: 'Support' },
  { skill: 'Rifles', name: 'Rifles', category: 'Combat' },
  { skill: 'Sniping', name: 'Sniping', category: 'Combat' },
  { skill: 'Camouflage', name: 'Camouflage', category: 'Survival' },
  { skill: 'Survival', name: 'Survival', category: 'Survival' },
  { skill: 'Melee Weapons', name: 'Melee Weapons', category: 'Combat' },
  { skill: 'Handgun', name: 'Handgun', category: 'Combat' },
  { skill: 'Running', name: 'Running', category: 'Physical' },
  { skill: 'Endurance', name: 'Endurance', category: 'Physical' },
  { skill: 'Tactics', name: 'Tactics', category: 'Combat' },
  { skill: 'Cooking', name: 'Cooking', category: 'Survival' },
  { skill: 'Thievery', name: 'Thievery', category: 'Support' },
  { skill: 'Archery', name: 'Archery', category: 'Combat' },
  { skill: 'Driving', name: 'Driving', category: 'Vehicle' },
  { skill: 'Engineering', name: 'Engineering', category: 'Craft' },
  { skill: 'Demolition', name: 'Demolition', category: 'Craft' },
  { skill: 'Medical', name: 'Medical', category: 'Support' },
  { skill: 'Motorcycling', name: 'Motorcycling', category: 'Vehicle' },
  { skill: 'Stealth', name: 'Stealth', category: 'Physical' },
  { skill: 'Aviation', name: 'Aviation', category: 'Vehicle' },
  { skill: 'Farming', name: 'Farming', category: 'Survival' }
];

function mergeSkillCatalog(skills) {
  const rows = Array.isArray(skills) ? skills.slice() : [];
  const seen = new Set(rows.map(skill => String(skill.key || skill.skill || skill.name || '').toLowerCase().replace(/[\s_"'-]+/g, '')));
  for (const skill of FULL_SCUM_SKILLS) {
    const key = String(skill.skill || skill.name || '').toLowerCase().replace(/[\s_"'-]+/g, '');
    if (!seen.has(key)) {
      rows.push(skill);
      seen.add(key);
    }
  }
  return rows;
}

function isAllSkillsValue(value) {
  return ['allskills', 'allskill', 'fullskills', 'maxskills', 'всенавыки', 'все'].includes(
    String(value || '').trim().toLowerCase().replace(/[\s_"'.-]+/g, '')
  );
}

function skillOptionHtml(skill) {
  return `<option value="${escapeAttr(skill.key || skill.skill || skill.name || '')}">${escapeHtml(skill.name || skill.category || skill.key || '')}</option>`;
}

function configLabel(key) {
  if (configLabels[key]) return configLabels[key];
  const match = Object.keys(configLabels).find(labelKey => labelKey.toLowerCase() === String(key).toLowerCase());
  if (match) return configLabels[match];
  return humanConfigLabel(key);
}

const configWordLabels = {
  enabled: 'включено',
  enable: 'включить',
  disabled: 'выключено',
  name: 'название',
  title: 'название',
  description: 'описание',
  message: 'сообщение',
  success: 'успех',
  error: 'ошибка',
  warning: 'предупреждение',
  cooldown: 'кулдаун',
  delay: 'задержка',
  inter: 'между',
  interval: 'интервал',
  ms: 'мс',
  seconds: 'сек',
  second: 'сек',
  minutes: 'мин',
  minute: 'мин',
  hours: 'ч',
  hour: 'ч',
  max: 'макс.',
  min: 'мин.',
  default: 'по умолчанию',
  serialize: 'сохранять',
  globally: 'глобально',
  claim: 'получение',
  claims: 'получения',
  required: 'обязательно',
  permission: 'право',
  player: 'игрок',
  players: 'игроки',
  steam: 'Steam',
  steamid: 'SteamID',
  id: 'ID',
  api: 'API',
  key: 'ключ',
  token: 'токен',
  url: 'URL',
  http: 'HTTP',
  timeout: 'таймаут',
  webhook: 'Webhook',
  channel: 'канал',
  discord: 'Discord',
  server: 'сервер',
  source: 'источник',
  prefix: 'префикс',
  use: 'использовать',
  runtime: 'runtime',
  hook: 'хук',
  hooks: 'хуки',
  broadcast: 'объявлять',
  fanout: 'рассылка',
  duplicate: 'дубликат',
  window: 'окно',
  threshold: 'порог',
  board: 'таблица',
  entries: 'записи',
  entry: 'запись',
  hero: 'герой',
  leader: 'лидер',
  kill: 'убийство',
  kills: 'убийства',
  death: 'смерть',
  deaths: 'смерти',
  suicide: 'самоубийство',
  suicides: 'самоубийства',
  weapon: 'оружие',
  distance: 'дистанция',
  reward: 'награда',
  rewards: 'награды',
  item: 'предмет',
  items: 'предметы',
  vehicle: 'транспорт',
  vehicles: 'транспорт',
  amount: 'сумма',
  price: 'цена',
  cost: 'стоимость',
  money: 'деньги',
  gold: 'золото',
  skill: 'навык',
  skills: 'навыки',
  attributes: 'атрибуты',
  strength: 'сила',
  constitution: 'телосложение',
  dexterity: 'ловкость',
  intelligence: 'интеллект',
  command: 'команда',
  commands: 'команды',
  template: 'шаблон',
  queue: 'очередь',
  length: 'длина',
  limit: 'лимит',
  route: 'маршрут',
  routes: 'маршруты',
  point: 'точка',
  points: 'точки',
  home: 'дом',
  homes: 'дома',
  vip: 'VIP',
  rental: 'аренда',
  rent: 'аренда',
  penalty: 'штраф',
  missing: 'потеря',
  scan: 'скан',
  include: 'показывать',
  ignore: 'игнорировать',
  notify: 'уведомлять',
  forward: 'пересылать',
  filter: 'фильтр',
  mode: 'режим',
  type: 'тип',
  label: 'метка',
  count: 'количество',
  quantity: 'количество'
};

function humanConfigLabel(key) {
  const raw = String(key || '').trim();
  if (!raw) return '';
  const words = raw
    .replace(/\./g, ' ')
    .replace(/[_-]+/g, ' ')
    .replace(/([a-zа-я0-9])([A-ZА-Я])/g, '$1 $2')
    .split(/\s+/)
    .filter(Boolean);
  const translated = words.map(word => {
    const normalized = word.toLowerCase().replace(/[^a-zа-я0-9]/gi, '');
    return configWordLabels[normalized] || word;
  });
  return translated.join(' ').replace(/^./, letter => letter.toUpperCase());
}

function selectOptionLabel(value) {
  return selectLabels[value] || value;
}

function categorySymbol(entry, kind) {
  if (kind === 'vehicle') return 'CAR';
  const text = String(entry.category || entry.name || entry.itemId || '').toLowerCase();
  if (/weapon|rifle|pistol|shotgun|bow|melee/.test(text)) return 'WPN';
  if (/ammo|magazine|bullet|cartridge/.test(text)) return 'AMO';
  if (/food|drink|water|meat|fruit|vegetable/.test(text)) return 'FOD';
  if (/medical|bandage|pill|syringe|antibiotic/.test(text)) return 'MED';
  if (/clothing|jacket|pants|boots|helmet|vest/.test(text)) return 'EQP';
  if (/tool|repair|kit|lockpick/.test(text)) return 'TLS';
  return shortCode(entry.name || entry.itemId, 'IT');
}

function catalogIconHtml(entry, kind) {
  const url = getCatalogIconUrl(entry || {}, kind) || generatedCatalogIconUrl(entry || {}, kind);
  const symbol = categorySymbol(entry || {}, kind);
  if (url) {
    return `<span class="catalog-icon ${escapeAttr(kind)} has-image"><img src="${escapeAttr(url)}" alt="${escapeAttr(symbol)}" loading="lazy" /></span>`;
  }
  return `<span class="catalog-icon ${escapeAttr(kind)}">${escapeHtml(symbol)}</span>`;
}

function catalogTile(entry, kind) {
  const value = catalogValue(entry, kind);
  const name = catalogDisplayName(entry, kind);
  return `<button type="button" class="catalog-tile" data-catalog-kind="${escapeAttr(kind)}" data-catalog-value="${escapeAttr(value)}" data-catalog-name="${escapeAttr(name)}" data-catalog-category="${escapeAttr(entry.category || '')}" title="${escapeAttr(value)}">
    ${catalogIconHtml(entry, kind)}
    <span class="catalog-text"><b>${escapeHtml(name)}</b><small>${escapeHtml(value)}</small></span>
    <span class="catalog-category">${escapeHtml(entry.category || '')}</span>
  </button>`;
}

function playerInventoryKey(steamId, name, runtimeKey = '') {
  return steamId || (name ? `name:${name}` : (runtimeKey ? `runtime:${runtimeKey}` : 'selected'));
}

function inventorySlotLabel(row) {
  const kind = String(row.slotKind || '').toLowerCase();
  if (kind === 'hands') return 'В руках';
  if (kind === 'equipped') return 'Экипировка';
  if (kind === 'container') return 'Контейнер';
  if (kind === 'root') return 'Корень';
  return row.slot == null ? '-' : `Слот ${row.slot}`;
}

function renderInventoryNode(row, childrenByContainer, depth = 0, visited = new Set()) {
  const entityId = String(row.entityId || '');
  if (visited.has(entityId)) return '';
  visited.add(entityId);
  const itemId = row.itemId || cleanAssetId(row.itemClass || row.itemEntitySetup || '');
  const display = friendlyAssetName(itemId || row.itemClass || row.itemEntitySetup, 'item');
  const iconEntry = {
    itemId,
    name: display,
    runtimeId: row.runtimeId || row.itemClass || row.itemEntitySetup || '',
    classPath: row.itemClass || '',
    assetPath: row.itemEntitySetup || '',
    icon: row.icon || row.Icon || row.iconName || '',
    category: row.category || ''
  };
  const children = childrenByContainer.get(entityId) || [];
  const indent = Math.min(depth, 5) * 14;
  const deleteButton = entityId ? `<button type="button" class="mini-action danger inventory-delete-btn"
        data-inventory-delete="${escapeAttr(entityId)}"
        data-inventory-item-id="${escapeAttr(itemId || '')}"
        data-inventory-class-path="${escapeAttr(row.itemClass || '')}"
        data-inventory-asset-path="${escapeAttr(row.itemEntitySetup || '')}">Удалить</button>` : '';
  return `<div class="inventory-node" style="--depth:${indent}px">
    <div class="inventory-node-main">
      ${catalogIconHtml(iconEntry, 'item')}
      <div><b>${escapeHtml(display)}</b><small>${escapeHtml(itemId || row.itemClass || '')}</small></div>
      <span class="inventory-chip">${escapeHtml(inventorySlotLabel(row))}</span>
      ${deleteButton}
    </div>
    ${children.length ? `<div class="inventory-children">${children.map(child => renderInventoryNode(child, childrenByContainer, depth + 1, new Set(visited))).join('')}</div>` : ''}
  </div>`;
}

function renderInventoryTree(rows) {
  const safeRows = Array.isArray(rows) ? rows : [];
  if (!safeRows.length) return '<div class="muted-line">Инвентарь пока не загружен.</div>';
  const playerEntityId = String(safeRows[0].playerEntityId || '');
  const byContainer = new Map();
  for (const row of safeRows) {
    const key = String(row.containerEntityId || '');
    if (!byContainer.has(key)) byContainer.set(key, []);
    byContainer.get(key).push(row);
  }
  const roots = byContainer.get(playerEntityId) || safeRows.filter(row => Number(row.depth || 0) === 0);
  return roots.length ? roots.map(row => renderInventoryNode(row, byContainer)).join('') : '<div class="muted-line">Корневые слоты не найдены.</div>';
}

function setPage(page, refresh = true) {
  if (page === 'map') {
    page = 'servers';
    toast('Карта отключена, чтобы не нагружать игровой сервер.');
  }
  if (page === 'diagnostics') {
    page = 'servers';
    toast('Диагностика отключена, чтобы не нагружать игровой сервер.');
  }
  if (page === 'economy') {
    page = 'players';
    toast('Отдельная экономика отключена. Баланс меняется из карточки игрока.');
  }
  state.page = page;
  document.querySelectorAll('.nav-item').forEach(btn => btn.classList.toggle('active', btn.dataset.page === page));
  document.querySelectorAll('.page').forEach(section => section.classList.toggle('active', section.id === `page-${page}`));
  const title = t(`pages.${page}`, page);
  els.pageTitle.textContent = title;
  els.crumb.textContent = t('platform', 'Платформа');
  if (refresh) refreshCurrent();
}

function setServiceTab(tab) {
  state.serviceTab = tab || 'player';
  document.querySelectorAll('[data-service-tab]').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.serviceTab === state.serviceTab);
  });
  document.querySelectorAll('[data-service-section]').forEach(section => {
    section.hidden = section.dataset.serviceSection !== state.serviceTab;
  });
}

function setEconomyTab(tab) {
  state.economyTab = tab || 'overview';
  document.querySelectorAll('[data-economy-tab]').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.economyTab === state.economyTab);
  });
  document.querySelectorAll('[data-economy-section]').forEach(section => {
    section.hidden = section.dataset.economySection !== state.economyTab;
  });
}

function statusOnlinePlayerCount(status) {
  const counters = status && status.performance && status.performance.counters;
  const raw = counters && (counters.P ?? counters.p ?? counters.players ?? counters.playerCount);
  const count = Number(raw);
  return Number.isFinite(count) && count >= 0 ? Math.trunc(count) : null;
}

function displayedOnlinePlayerCount(status) {
  const cachedCount = Array.isArray(state.players) ? state.players.length : 0;
  const cacheAgeMs = Date.now() - Number(state.playersLastRefreshedAt || 0);
  if (state.playersLastRefreshedAt && cacheAgeMs < 15000) return cachedCount;
  const statusCount = statusOnlinePlayerCount(status);
  return statusCount == null ? cachedCount : statusCount;
}

function refreshStatus() {
  if (state.statusRefreshInFlight) return state.statusRefreshInFlight;
  state.statusRefreshInFlight = (async () => {
  const status = await api('/api/status');
  const bridgeOnline = status.bridge && status.bridge.online;
  const playerCount = displayedOnlinePlayerCount(status);
  const diagnosticsProblem = detectDiagnosticsProblem(status, bridgeOnline);
  updateDiagnosticSupport(status, diagnosticsProblem);
  els.bridgePill.textContent = `${t('bridge', 'мост')}: ${bridgeOnline ? t('online', 'в сети') : t('noConnection', 'нет связи')}`;
  els.bridgePill.className = `pill ${bridgeOnline ? 'good' : 'bad'}`;
  if (els.playersPill) els.playersPill.textContent = `${t('players', 'игроки')}: ${playerCount}`;
  if (els.dashboardServerName) els.dashboardServerName.textContent = status.server || 'SCUM NEDJIN';
  if (els.dashboardStatusText) {
    const statusLabel = bridgeOnline ? `${t('online', 'в сети')} (${playerCount})` : t('noConnection', 'нет связи');
    els.dashboardStatusText.textContent = statusLabel;
    els.dashboardStatusText.className = bridgeOnline ? 'dashboard-online' : 'dashboard-offline';
  }
  if (els.serverStatusDot) els.serverStatusDot.classList.toggle('online', !!bridgeOnline);
  if (els.serverStatusText) els.serverStatusText.textContent = bridgeOnline ? t('online', 'в сети') : t('offline', 'не в сети');
  const cards = [
    [t('bridge', 'Мост'), bridgeOnline ? t('online', 'в сети') : t('noConnection', 'нет связи'), status.bridge.message, bridgeOnline ? 'good' : 'bad'],
    ['Игровые данные', status.database.online ? t('readable', 'читаются') : t('missing', 'ожидаются'), status.database.online ? 'данные доступны' : 'ожидает файлы игры', status.database.online ? 'good' : 'warn'],
    ['Журнал сервера', status.logs.online ? t('readable', 'читается') : t('missing', 'ожидается'), status.logs.online ? 'события доступны' : 'ждёт первые события', status.logs.online ? 'good' : 'warn'],
    [t('server', 'Сервер'), status.server || 'SCUM', t('connectedHost', 'подключённая площадка'), 'good']
  ];
  const perf = status.performance || {};
  if (perf.online) {
    const perfMs = Number(perf.maxFrameMs || perf.frameMs || 0);
    const frameText = perf.frameMs != null ? `${perf.frameMs} ms / ${perf.fps || '?'} FPS` : 'Global Stats найден';
    const frameNote = perf.maxFrameMs != null ? `sample3 ${perf.maxFrameMs} ms; это frame time, не ping` : (perf.note || 'это frame time сервера, не ping');
    cards.splice(3, 0, ['Серверный кадр', frameText, frameNote, perfMs >= 200 ? 'warn' : 'good']);
  }
  els.statusGrid.innerHTML = cards.map(([name, value, note, kind]) => `
    <article class="metric ${kind}">
      <span>${escapeHtml(name)}</span>
      <b>${escapeHtml(value)}</b>
      <span>${escapeHtml(note || '')}</span>
    </article>`).join('');
  })().finally(() => {
    state.statusRefreshInFlight = null;
  });
  return state.statusRefreshInFlight;
}

async function refreshServers() {
  const servers = await api('/api/servers').catch(() => []);
  state.servers = Array.isArray(servers) ? servers : [];
  const panelBase = state.apiBase || window.location.origin;
  const panelUrl = (() => {
    try { return new URL(panelBase); } catch (_) { return new URL(window.location.origin); }
  })();
  const isLoopbackPanel = panelUrl.hostname === '127.0.0.1' || panelUrl.hostname === 'localhost' || panelUrl.hostname === '::1';
  const formatGameEndpoint = server => {
    const host = server.gameHost || server.publicHost || server.host || server.address || (isLoopbackPanel ? '127.0.0.1' : panelUrl.hostname);
    const port = server.gamePort || server.serverPort || server.game_port || server.port || (isLoopbackPanel ? '7777' : '');
    return port ? `${host}:${port}` : host;
  };
  if (els.serverSelect) {
    els.serverSelect.innerHTML = state.servers.length
      ? state.servers.map(server => `<option value="${escapeAttr(server.id || server.name || '')}">${escapeHtml(serverDisplayName(server))}</option>`).join('')
      : '<option value="">SCUM сервер</option>';
  }
  if (els.serverList) {
    els.serverList.innerHTML = state.servers.length ? state.servers.map(server => `<article class="server-card">
      <div class="server-card-head">
        <b>${escapeHtml(serverDisplayName(server))}</b>
        <span class="pill ${server.online === false ? 'bad' : 'good'}">${server.online === false ? 'выключен' : 'работает'}</span>
      </div>
      <div class="server-kv">
        <span>Игровой сервер</span><b>${escapeHtml(formatGameEndpoint(server))}</b>
        <span>Панель / API</span><b>${escapeHtml(panelBase)}</b>
        <span>Доступ</span><b>${escapeHtml(isLoopbackPanel ? 'локально, только этот ПК' : 'сетевой адрес')}</b>
      </div>
      <div class="button-row">
        <button class="btn" data-server-action="players">Игроки</button>
        <button class="btn" data-server-action="modules">Плагины</button>
      </div>
    </article>`).join('') : `<article class="server-card">
      <div class="server-card-head"><b>SCUM сервер</b><span class="pill">настроен</span></div>
      <div class="server-kv">
        <span>API</span><b>${escapeHtml(state.apiBase || window.location.origin)}</b>
        <span>Ключ</span><b>${escapeHtml(state.apiKey ? 'задан' : 'не задан')}</b>
        <span>Мост</span><b>${escapeHtml(els.bridgePill ? els.bridgePill.textContent.replace('мост: ', '').replace('bridge: ', '') : '')}</b>
      </div>
    </article>`;
  }
}

function serverDisplayName(server) {
  return server && (server.name || server.server || server.serverName) || 'SCUM сервер';
}

async function refreshServerControlStatus() {
  if (!els.serverControlStatus) return;
  const status = await api('/api/server-control/status').catch(err => ({ configured: false, error: err && err.message ? err.message : String(err) }));
  const configured = !!(status && status.configured);
  els.serverControlStatus.textContent = configured ? 'host-control настроен' : 'host-control не настроен';
  els.serverControlStatus.className = `pill ${configured ? 'good' : 'warn'}`;
  for (const btn of document.querySelectorAll('[data-server-control-action]')) {
    if (!btn.dataset.originalTitle) btn.dataset.originalTitle = btn.title || '';
    btn.disabled = !configured;
    btn.title = configured
      ? btn.dataset.originalTitle
      : 'Настрой ark_panel_* в ScumNeDjin/nedjin.ini, чтобы управлять сервером из панели.';
  }
}

async function runServerControlAction(action) {
  const clean = String(action || '').trim().toLowerCase();
  if (!clean) return;
  const confirmText = clean === 'stop'
    ? 'Остановить сервер? Если панель работает внутри SCUMServer.exe, после остановки она станет недоступна.'
    : clean === 'restart'
      ? 'Перезапустить сервер через панель хостинга?'
      : 'Запустить сервер через панель хостинга?';
  if (!window.confirm(confirmText)) return;
  await showActionResult(els.serverControlResult, () => api('/api/server-control', {
    method: 'POST',
    body: JSON.stringify({ action: clean })
  }));
}

function renderServerConfigList() {
  if (!els.serverConfigList) return;
  const files = Array.isArray(state.serverConfigs) ? state.serverConfigs : [];
  els.serverConfigList.innerHTML = files.length ? files.map(file => {
    const name = file.name || '';
    const active = name && name === state.selectedServerConfig;
    const exists = file.exists !== false;
    return `<button class="server-config-item ${active ? 'active' : ''}" type="button" data-server-config="${escapeAttr(name)}">
      <span>${escapeHtml(name || 'config')}</span>
      <small>${exists ? `${escapeHtml(file.type || '')} · ${Number(file.sizeBytes || 0)} B` : 'будет создан'}</small>
    </button>`;
  }).join('') : '<div class="empty">SCUM config файлы не найдены.</div>';
}

function resizeServerConfigContent() {
  const input = els.serverConfigContent;
  if (!input) return;
  input.style.setProperty('height', 'auto', 'important');
  const narrow = typeof window.matchMedia === 'function' && window.matchMedia('(max-width: 760px)').matches;
  const minHeight = narrow ? 380 : 560;
  const height = Math.max(minHeight, Math.ceil(input.scrollHeight || 0) + 8);
  input.style.setProperty('height', `${height}px`, 'important');
}

function queueServerConfigResize() {
  if (typeof window.requestAnimationFrame === 'function') {
    window.requestAnimationFrame(resizeServerConfigContent);
    return;
  }
  setTimeout(resizeServerConfigContent, 0);
}

function sameServerConfigName(left, right) {
  return String(left || '').trim().toLowerCase() === String(right || '').trim().toLowerCase();
}

function updateServerConfigSaveState() {
  if (!els.serverConfigSave) return;
  const name = String(state.selectedServerConfig || '').trim();
  const content = els.serverConfigContent ? String(els.serverConfigContent.value || '') : '';
  const loadedForSelection = name.length > 0 && sameServerConfigName(state.serverConfigLoadedName, name);
  const nonEmptyContent = content.trim().length > 0;
  const disabled = state.serverConfigLoading || !loadedForSelection || !nonEmptyContent;
  els.serverConfigSave.disabled = disabled;
  els.serverConfigSave.title = state.serverConfigLoading
    ? 'Дождитесь завершения чтения выбранного конфига.'
    : !loadedForSelection
      ? 'Сначала успешно загрузите выбранный конфиг.'
      : !nonEmptyContent
        ? 'Пустой конфиг нельзя сохранить через панель.'
        : '';
}

async function refreshServerConfigs() {
  if (!els.serverConfigList) return;
  const data = await api('/api/server-configs').catch(err => ({ files: [], error: err && err.message ? err.message : String(err) }));
  state.serverConfigs = Array.isArray(data && data.files) ? data.files : [];
  if (!state.selectedServerConfig && state.serverConfigs.length) {
    const preferred = state.serverConfigs.find(file => String(file.name || '').toLowerCase() === 'serversettings.ini') || state.serverConfigs[0];
    state.selectedServerConfig = preferred.name || '';
  }
  renderServerConfigList();
  updateServerConfigSaveState();
  if (state.selectedServerConfig && els.serverConfigContent && !els.serverConfigContent.value && !state.serverConfigLoadedName) {
    await loadServerConfig(state.selectedServerConfig).catch(() => {});
  }
}

async function loadServerConfig(name) {
  const clean = String(name || state.selectedServerConfig || '').trim();
  if (!clean) return;
  const requestVersion = ++state.serverConfigLoadVersion;
  state.selectedServerConfig = clean;
  state.serverConfigLoading = true;
  state.serverConfigLoadedName = '';
  state.serverConfigLoadedContent = null;
  if (els.serverConfigTitle) els.serverConfigTitle.textContent = clean;
  renderServerConfigList();
  updateServerConfigSaveState();
  try {
    const data = await api(`/api/server-config?name=${encodeURIComponent(clean)}`);
    if (requestVersion !== state.serverConfigLoadVersion || !sameServerConfigName(state.selectedServerConfig, clean)) return;
    const loadedName = String(data && data.name || clean).trim() || clean;
    const content = String(data && data.content || '');
    state.selectedServerConfig = loadedName;
    state.serverConfigLoadedName = loadedName;
    state.serverConfigLoadedContent = content;
    if (els.serverConfigTitle) els.serverConfigTitle.textContent = loadedName;
    if (els.serverConfigContent) els.serverConfigContent.value = content;
    queueServerConfigResize();
    showResult(els.serverConfigResult, `Загружен ${loadedName}. Изменения применятся после рестарта.`);
    renderServerConfigList();
  } catch (err) {
    if (requestVersion === state.serverConfigLoadVersion) {
      state.serverConfigLoadedName = '';
      state.serverConfigLoadedContent = null;
    }
    throw err;
  } finally {
    if (requestVersion === state.serverConfigLoadVersion) {
      state.serverConfigLoading = false;
      updateServerConfigSaveState();
    }
  }
}

async function saveServerConfig() {
  const name = String(state.selectedServerConfig || '').trim();
  if (!name) {
    showResult(els.serverConfigResult, 'Выберите конфиг.');
    return;
  }
  if (state.serverConfigLoading || !sameServerConfigName(state.serverConfigLoadedName, name) || state.serverConfigLoadedContent === null) {
    showResult(els.serverConfigResult, 'Сохранение заблокировано: сначала успешно загрузите выбранный конфиг.');
    updateServerConfigSaveState();
    return;
  }
  const content = els.serverConfigContent ? String(els.serverConfigContent.value || '') : '';
  if (!content.trim()) {
    showResult(els.serverConfigResult, 'Пустой конфиг не сохранён: это защищает текущие настройки сервера.');
    updateServerConfigSaveState();
    return;
  }
  try {
    showResult(els.serverConfigResult, 'Проверяю актуальность конфига...');
    const current = await api(`/api/server-config?name=${encodeURIComponent(name)}`);
    const currentName = String(current && current.name || name).trim() || name;
    const currentContent = String(current && current.content || '');
    if (!sameServerConfigName(currentName, name) || currentContent !== state.serverConfigLoadedContent) {
      state.serverConfigLoadedName = '';
      state.serverConfigLoadedContent = null;
      showResult(els.serverConfigResult, 'Сохранение отменено: конфиг изменился после загрузки. Перечитайте его и повторите правку вручную.');
      return;
    }
    showResult(els.serverConfigResult, 'Сохраняю...');
    const saved = await api('/api/server-config', {
      method: 'POST',
      body: JSON.stringify({ name, content })
    });
    state.serverConfigLoadedName = name;
    state.serverConfigLoadedContent = content;
    showResult(els.serverConfigResult, saved);
  } catch (err) {
    showResult(els.serverConfigResult, { ok: false, error: err && err.message ? err.message : String(err) });
  } finally {
    updateServerConfigSaveState();
  }
  await refreshServerConfigs().catch(() => {});
}

function setServerConfigExpanded(expanded) {
  state.serverConfigExpanded = Boolean(expanded);
  if (els.serverConfigPanel) els.serverConfigPanel.classList.toggle('expanded', state.serverConfigExpanded);
  if (els.serverConfigExpand) {
    els.serverConfigExpand.textContent = state.serverConfigExpanded ? '↙' : '⛶';
    els.serverConfigExpand.title = state.serverConfigExpanded ? 'Свернуть редактор' : 'Развернуть редактор';
    els.serverConfigExpand.setAttribute('aria-pressed', state.serverConfigExpanded ? 'true' : 'false');
  }
  if (state.serverConfigExpanded && els.serverConfigContent) {
    setTimeout(() => els.serverConfigContent.focus(), 0);
  }
  queueServerConfigResize();
}

function toggleServerConfigExpanded() {
  setServerConfigExpanded(!state.serverConfigExpanded);
}

async function refreshPlayers() {
  if (state.playersRefreshInFlight) return state.playersRefreshInFlight;
  const playersUrl = '/api/players?mode=log';
  state.playersRefreshInFlight = api(playersUrl).finally(() => {
    state.playersRefreshInFlight = null;
  });
  const data = await state.playersRefreshInFlight;
  const rawPlayers = playersFromPayload(data);
  const players = rawPlayers
    .map((player, index) => normalizePlayerForDisplay(player, index))
    .filter(Boolean);
  state.hiddenRuntimePlayers = rawPlayers.length - players.length;
  state.runtimeOnlyPlayers = players.filter(isRuntimeOnlyPlayer).length;
  state.players = players;
  state.playersLastRefreshedAt = Date.now();
  if (state.selectedPlayerTarget) {
    const selected = state.selectedPlayerTarget;
    const matched = players.find(p => {
      const steamId = String(p.steamId || p.SteamId || p.steam || '');
      const name = String(p.name || p.Name || p.playerName || '');
      return (selected.steamId && steamId === selected.steamId) ||
        (selected.name && name.toLowerCase() === selected.name.toLowerCase());
    });
    if (matched) {
      selected.runtimeKey = matched.runtimeKey || matched.RuntimeKey || selected.runtimeKey || '';
      selected.steamId = matched.steamId || matched.SteamId || matched.steam || selected.steamId || '';
      selected.name = matched.name || matched.Name || matched.playerName || selected.name || '';
      selected.profileId = matched.profileId || matched.ProfileId || matched.userProfileId || matched.serverUserProfileId || selected.profileId || '';
    }
  }
  els.playersPill.textContent = `${t('players', 'игроки')}: ${players.length}`;
  renderPlayers();
}

function playersFromPayload(data) {
  if (Array.isArray(data)) return data;
  const candidates = [
    data,
    data && data.payload,
    data && data.data,
    data && data.payload && data.payload.data,
    data && data.data && data.data.data,
    data && data.payload && data.payload.payload
  ].filter(Boolean);
  for (const payload of candidates) {
    if (Array.isArray(payload)) return payload.slice();
    if (payload && Array.isArray(payload.players)) return payload.players.slice();
    if (payload && payload.result && Array.isArray(payload.result.players)) return payload.result.players.slice();
  }
  return [];
}

function cleanPlayerString(value) {
  return String(value == null ? '' : value).trim();
}

function isPlaceholderPlayerName(name) {
  const value = cleanPlayerString(name).toLowerCase();
  return !value ||
    value === 'unknown' ||
    /^unknown\s+live(?:\s+#\d+)?$/i.test(value) ||
    /^live\s+#\d+$/i.test(value);
}

function isRuntimeOnlyPlayer(player) {
  if (!player) return false;
  const source = cleanPlayerString(pick(player, ['source', 'identitySource'], '')).toLowerCase();
  const steamId = cleanPlayerString(pick(player, ['steamId', 'SteamId', 'steam'], ''));
  const name = cleanPlayerString(pick(player, ['name', 'Name', 'playerName'], ''));
  const profileId = cleanPlayerString(pick(player, ['userProfileId', 'serverUserProfileId', 'profileId', 'ProfileId'], ''));
  const runtimeKey = cleanPlayerString(pick(player, ['runtimeKey', 'RuntimeKey'], ''));
  const liveSource = source.includes('ue4ss') || source.includes('runtime') || source.includes('live');
  const missingIdentity = !(steamId && steamId !== '0') && !(profileId && profileId !== '0') && isPlaceholderPlayerName(name);
  return Boolean(player.runtimeOnly) || source.includes('runtime-only') || (missingIdentity && Boolean(runtimeKey || liveSource));
}

function isRenderablePlayer(player) {
  if (!player) return false;
  const steamId = cleanPlayerString(pick(player, ['steamId', 'SteamId', 'steam'], ''));
  const name = cleanPlayerString(pick(player, ['name', 'Name', 'playerName'], ''));
  const profileId = cleanPlayerString(pick(player, ['userProfileId', 'serverUserProfileId', 'profileId', 'ProfileId'], ''));
  const hasSteam = Boolean(steamId && steamId !== '0');
  const hasProfile = Boolean(profileId && profileId !== '0');
  const hasName = Boolean(name && !isPlaceholderPlayerName(name));
  return hasSteam || hasProfile || hasName || isRuntimeOnlyPlayer(player);
}

function normalizePlayerForDisplay(player, index = 0) {
  if (!isRenderablePlayer(player)) return null;
  const normalized = Object.assign({}, player);
  const name = cleanPlayerString(pick(normalized, ['name', 'Name', 'playerName'], ''));
  if (isPlaceholderPlayerName(name) && isRuntimeOnlyPlayer(normalized)) {
    const runtimeKey = cleanPlayerString(pick(normalized, ['runtimeKey', 'RuntimeKey'], ''));
    const suffix = runtimeKey ? runtimeKey.slice(-6) : String(index + 1);
    normalized.name = `Unknown live #${suffix}`;
    normalized.runtimeOnly = true;
    normalized.identitySource = cleanPlayerString(normalized.identitySource || 'runtime-only');
  }
  return normalized;
}

function playerHasMapPosition(player) {
  const source = String(pick(player, ['positionSource', 'source'], '') || '').toLowerCase();
  if (source.includes('no-location')) return false;
  const x = Number(pick(player, ['x', 'X', 'locationX', 'LocationX'], NaN));
  const y = Number(pick(player, ['y', 'Y', 'locationY', 'LocationY'], NaN));
  return Number.isFinite(x) && Number.isFinite(y) && Math.abs(x) + Math.abs(y) > 1;
}

function renderPlayers() {
  const q = (els.playerSearch.value || '').toLowerCase();
  const sort = els.playersSort ? els.playersSort.value : 'name';
  const filtered = state.players.filter(p => JSON.stringify(p).toLowerCase().includes(q));
  filtered.sort((a, b) => {
    if (sort === 'money') return Number(pick(b, ['walletBalance', 'money', 'balance'], 0)) - Number(pick(a, ['walletBalance', 'money', 'balance'], 0));
    if (sort === 'fame') return Number(pick(b, ['famePoints', 'fame'], 0)) - Number(pick(a, ['famePoints', 'fame'], 0));
    if (sort === 'login') return String(pick(b, ['lastLoginUtc', 'lastSeen', 'login'], '')).localeCompare(String(pick(a, ['lastLoginUtc', 'lastSeen', 'login'], '')));
    return String(pick(a, ['name', 'Name', 'playerName'], '')).localeCompare(String(pick(b, ['name', 'Name', 'playerName'], '')));
  });
  if (els.playersSummary) {
    els.playersSummary.innerHTML = `
      <article class="summary-card"><b>${state.players.length}</b><span>онлайн</span></article>
      <article class="summary-card"><b>${filtered.length}</b><span>в фильтре</span></article>
      <article class="summary-card"><b>${state.runtimeOnlyPlayers || 0}</b><span>runtime без SteamID</span></article>`;
  }
  els.playersList.innerHTML = filtered.length ? filtered.map(playerCard).join('') : '<div class="panel">Сейчас серверный лог не подтверждает игроков онлайн.</div>';
  syncPlayerProfile();
}

function playerCard(p) {
  const name = pick(p, ['name', 'Name', 'playerName'], 'Игрок');
  const steamId = pick(p, ['steamId', 'SteamId', 'steam'], '');
  const runtimeKey = pick(p, ['runtimeKey', 'RuntimeKey'], '');
  const userProfileId = pick(p, ['userProfileId', 'serverUserProfileId', 'profileId', 'ProfileId'], '');
  const initials = String(name || 'SC').slice(0, 2).toUpperCase();
  const selected = Boolean(state.selectedPlayerTarget && (
    (steamId && steamId === state.selectedPlayerTarget.steamId) ||
    (runtimeKey && runtimeKey === state.selectedPlayerTarget.runtimeKey) ||
    (name && String(name).toLowerCase() === String(state.selectedPlayerTarget.name || '').toLowerCase())
  ));
  return `<article class="player player-row${selected ? ' selected' : ''}" tabindex="0" role="button" aria-selected="${selected ? 'true' : 'false'}" aria-label="Выбрать игрока ${escapeAttr(name)}" data-steam="${escapeAttr(steamId)}" data-name="${escapeAttr(name)}" data-runtime="${escapeAttr(runtimeKey)}" data-profile="${escapeAttr(userProfileId)}">
    <div class="player-main">
      <div class="player-avatar">${escapeHtml(initials)}</div>
      <div class="player-title">
        <b class="player-name-link">${escapeHtml(name)}</b>
        <small>${escapeHtml(steamId || 'SteamID не найден')}</small>
      </div>
      <span class="pill good player-status">онлайн</span>
    </div>
    <div class="player-meta-grid">
      <span><b>Профиль</b>${escapeHtml(userProfileId || '-')}</span>
      <span><b>Runtime</b>${escapeHtml(runtimeKey || '-')}</span>
    </div>
    <button class="player-row-open" type="button" data-act="modal" data-tab="items" data-steam="${escapeAttr(steamId)}" data-name="${escapeAttr(name)}" data-runtime="${escapeAttr(runtimeKey)}" data-profile="${escapeAttr(userProfileId)}">Открыть управление</button>
  </article>`;
}

function playerCoords(player) {
  const x = Number(player && (player.x ?? player.X ?? player.locationX ?? player.LocationX ?? 0));
  const y = Number(player && (player.y ?? player.Y ?? player.locationY ?? player.LocationY ?? 0));
  const z = Number(player && (player.z ?? player.Z ?? player.locationZ ?? player.LocationZ ?? 0));
  return {
    x: Number.isFinite(x) ? x : 0,
    y: Number.isFinite(y) ? y : 0,
    z: Number.isFinite(z) ? z : 0
  };
}

function findPlayerByIdentity(steamId, name, runtimeKey = '') {
  const steam = String(steamId || '');
  const playerName = String(name || '').toLowerCase();
  const runtime = String(runtimeKey || '');
  return state.players.find(p => {
    const pSteam = String(p.steamId || p.SteamId || p.steam || '');
    const pName = String(p.name || p.Name || p.playerName || '').toLowerCase();
    const pRuntime = String(p.runtimeKey || p.RuntimeKey || '');
    return (steam && pSteam === steam) ||
      (runtime && pRuntime === runtime) ||
      (playerName && pName === playerName);
  }) || null;
}

function isPlayerOnline(steamId, name, runtimeKey = '') {
  return Boolean(findPlayerByIdentity(steamId, name, runtimeKey));
}

function setProfileOnlineStatus(online) {
  const badge = els.profStatus || (els.playerProfile && els.playerProfile.querySelector('.profile-badges .badge'));
  if (!badge) return;
  badge.textContent = online ? 'онлайн' : 'оффлайн';
  badge.classList.toggle('online', online);
  badge.classList.toggle('offline', !online);
}

function setSelectedPlayerTarget(steamId, name, runtimeKey = '', profileId = '', player = null) {
  const onlinePlayer = findPlayerByIdentity(steamId, name, runtimeKey);
  const livePlayer = onlinePlayer || player || {};
  const coords = playerCoords(livePlayer);
  const selected = {
    steamId: steamId || pick(livePlayer, ['steamId', 'SteamId', 'steam'], '') || '',
    name: name || pick(livePlayer, ['name', 'Name', 'playerName'], '') || '',
    runtimeKey: runtimeKey || pick(livePlayer, ['runtimeKey', 'RuntimeKey'], '') || '',
    profileId: profileId || pick(livePlayer, ['userProfileId', 'serverUserProfileId', 'profileId', 'ProfileId'], '') || '',
    target: steamId || name || pick(livePlayer, ['steamId', 'SteamId', 'steam', 'name', 'Name', 'playerName'], '') || '',
    online: Boolean(onlinePlayer),
    x: coords.x,
    y: coords.y,
    z: coords.z
  };
  state.selectedPlayerTarget = selected;
  const target = selected.target || selected.steamId || selected.name || '';
  if (els.playerActionTarget) els.playerActionTarget.value = target;
  if (els.itemTarget) els.itemTarget.value = target;
  if (els.actionTarget) els.actionTarget.value = target;
  if (els.serviceTarget) els.serviceTarget.value = target;
  if (els.economyTarget) els.economyTarget.value = target;
  if (els.playerActionX) els.playerActionX.value = Math.round(coords.x);
  if (els.playerActionY) els.playerActionY.value = Math.round(coords.y);
  if (els.playerActionZ) els.playerActionZ.value = Math.round(coords.z);
  return selected;
}

function openPlayerProfile(playerOrTarget, options = {}) {
  if (!els.playerProfile || !playerOrTarget) return;
  const player = playerOrTarget || {};
  const name = pick(player, ['name', 'Name', 'playerName'], 'Игрок');
  const steamId = pick(player, ['steamId', 'SteamId', 'steam'], '');
  const runtimeKey = pick(player, ['runtimeKey', 'RuntimeKey'], '');
  const profileId = pick(player, ['userProfileId', 'serverUserProfileId', 'profileId', 'ProfileId'], '');
  const selected = setSelectedPlayerTarget(steamId, name, runtimeKey, profileId, player);
  const online = isPlayerOnline(selected.steamId, selected.name, selected.runtimeKey);
  const coords = playerCoords(findPlayerByIdentity(selected.steamId, selected.name, selected.runtimeKey) || player);
  const initials = String(selected.name || 'SC').slice(0, 2).toUpperCase();
  if (els.profAvatar) els.profAvatar.textContent = initials;
  if (els.profName) els.profName.textContent = selected.name || 'Игрок';
  setProfileOnlineStatus(online);
  if (els.profSteamId) els.profSteamId.textContent = selected.steamId || 'SteamID не найден';
  if (els.profLocation) els.profLocation.textContent = `${Math.round(coords.x)}, ${Math.round(coords.y)}, ${Math.round(coords.z)}`;
  if (els.profProfileId) els.profProfileId.textContent = selected.profileId || '-';
  if (els.profX) els.profX.value = Math.round(coords.x);
  if (els.profY) els.profY.value = Math.round(coords.y);
  if (els.profZ) els.profZ.value = Math.round(coords.z);
  if (els.profActionResult && options.clearResult !== false) els.profActionResult.textContent = '';
  refreshWelcomeTimers().catch(() => updatePlayerWelcomeControls());
  els.playerProfile.classList.remove('hidden');
  els.playerProfile.setAttribute('aria-hidden', 'false');
  const playersPage = document.getElementById('page-players');
  if (playersPage) playersPage.classList.add('profile-open');
  const dockOpen = els.profileActionDock && !els.profileActionDock.classList.contains('hidden') && isPlayerActionDocked();
  if (options.actions !== false && dockOpen) {
    const activeTab = (els.profileActionDock.querySelector('[data-player-tab].active') || {}).dataset || {};
    openPlayerActionModal(selected.steamId, selected.name, selected.runtimeKey, activeTab.playerTab || 'message', selected.profileId, { inline: true });
  }
  if (options.scroll !== false) els.playerProfile.scrollIntoView({ behavior: 'smooth', block: 'start' });
}

function syncPlayerProfile() {
  if (!els.playerProfile || els.playerProfile.classList.contains('hidden') || !state.selectedPlayerTarget) return;
  const selected = state.selectedPlayerTarget;
  const player = findPlayerByIdentity(selected.steamId, selected.name, selected.runtimeKey);
  if (player) openPlayerProfile(player, { scroll: false, clearResult: false, actions: false });
  else setProfileOnlineStatus(false);
}

function closePlayerProfile() {
  if (!els.playerProfile) return;
  els.playerProfile.classList.add('hidden');
  els.playerProfile.setAttribute('aria-hidden', 'true');
  if (els.profileActionDock) {
    els.profileActionDock.classList.add('hidden');
    els.profileActionDock.setAttribute('aria-hidden', 'true');
  }
  const playersPage = document.getElementById('page-players');
  if (playersPage) playersPage.classList.remove('profile-open');
}

function openSelectedPlayerModal(tab = 'message') {
  const selected = state.selectedPlayerTarget;
  if (!selected) {
    toast('Сначала выбери игрока.');
    return;
  }
  openPlayerActionModal(selected.steamId, selected.name, selected.runtimeKey, tab, selected.profileId);
}

async function loadStaticMapChests() {
  try {
    const response = await fetch('/map-chests.json', { cache: 'no-store' });
    if (!response.ok) return [];
    const payload = await response.json();
    if (Array.isArray(payload)) return payload;
    if (payload && Array.isArray(payload.chests)) return payload.chests;
  } catch (_) {}
  return [];
}

async function refreshMap() {
  const mapData = await api('/api/map');
  state.map = mapData || {};
  const apiChests = Array.isArray(state.map.chests)
    ? state.map.chests
    : (Array.isArray(state.map.containers) ? state.map.containers : []);
  if (!apiChests.length) {
    const staticChests = await loadStaticMapChests();
    if (staticChests.length) {
      state.map.chests = staticChests;
      state.map.chestSource = 'игровые данные';
    }
  }
  let livePlayers = Array.isArray(state.players) ? state.players.filter(playerHasMapPosition) : [];
  if (!livePlayers.length) {
    const playersAgeMs = Date.now() - Number(state.playersLastRefreshedAt || 0);
    if (playersAgeMs > 10000) {
      try {
        await refreshPlayers();
        livePlayers = Array.isArray(state.players) ? state.players.filter(playerHasMapPosition) : [];
      } catch (_) {}
    }
  }
  if (!Array.isArray(state.map.players) || !state.map.players.length) {
    state.map.players = livePlayers;
    state.map.playerSource = livePlayers.length ? 'cached players' : (state.map.playerSource || 'нет live-игроков');
  }
  const mapImageUrl = String(state.map.mapImageUrl || '').trim();
  if (mapImageUrl) {
    els.mapImage.src = mapImageUrl;
    els.mapImage.hidden = false;
  } else {
    els.mapImage.removeAttribute('src');
    els.mapImage.hidden = true;
  }
  renderMap();
}

function renderMap() {
  if (!state.map) return;
  const bounds = state.map.bounds || {};
  renderMapSectors();
  const markers = [];
  const rawMapPlayers = Array.isArray(state.map.players) && state.map.players.length
    ? state.map.players
    : (Array.isArray(state.players) ? state.players.filter(playerHasMapPosition) : []);
  const mapPlayers = rawMapPlayers.filter(playerHasMapPosition);
  const mapChests = state.map.chests || state.map.containers || [];
  if (els.layerPlayers.checked) for (const p of mapPlayers) markers.push(markerFor(p, bounds, 'player', pick(p, ['name', 'Name'], 'игрок')));
  if (els.layerVehicles.checked) for (const v of state.map.vehicles || []) markers.push(markerFor(v, bounds, 'vehicle', pick(v, ['type', 'class', 'asset', '_table'], 'транспорт')));
  if (els.layerChests && els.layerChests.checked) for (const c of mapChests) markers.push(markerFor(c, bounds, 'chest', pick(c, ['type', 'class', 'asset', '_table'], 'сундук')));
  if (els.layerFlags.checked) for (const f of state.map.flags || []) markers.push(markerFor(f, bounds, 'flag', pick(f, ['ownerName', 'name', '_table'], 'флаг')));
  els.mapMarkers.innerHTML = markers.join('');
  applyMapZoom();
  els.mapStats.innerHTML = `
    <div>Игроки: ${mapPlayers.length}</div>
    <div>Транспорт: ${(state.map.vehicles || []).length}</div>
    <div>Сундуки: ${mapChests.length}</div>
    <div>Флаги: ${(state.map.flags || []).length}</div>
    <div>Источник игроков: ${escapeHtml(state.map.playerSource || (mapPlayers.length ? 'cached players' : ''))}</div>
    ${state.map.chestSource ? `<div>Источник сундуков: ${escapeHtml(state.map.chestSource)}</div>` : ''}`;
  renderMapSelection();
}

function renderMapSectors() {
  if (!els.mapSectors) return;
  const rows = ['D', 'C', 'B', 'A', 'Z'];
  const cols = ['4', '3', '2', '1', '0'];
  els.mapSectors.innerHTML = rows.flatMap(row => cols.map(col => `<div class="map-sector-cell"><span>${row}${col}</span></div>`)).join('');
}

function applyMapZoom() {
  const zoom = Math.max(0.6, Math.min(2.6, state.mapZoom || 1));
  state.mapZoom = zoom;
  if (els.mapImage) els.mapImage.style.transform = `scale(${zoom})`;
  if (els.mapMarkers) els.mapMarkers.style.transform = `scale(${zoom})`;
  if (els.mapSectors) els.mapSectors.style.transform = `scale(${zoom})`;
}

function markerFor(obj, bounds, kind, title) {
  const rawX = Number(pick(obj, ['x', 'X', 'locationX', 'LocationX'], NaN));
  const rawY = Number(pick(obj, ['y', 'Y', 'locationY', 'LocationY'], NaN));
  if (!Number.isFinite(rawX) || !Number.isFinite(rawY)) return '';
  let x = bounds.swapAxes ? rawY : rawX;
  let y = bounds.swapAxes ? rawX : rawY;
  const minX = Number(bounds.minX ?? -768000);
  const maxX = Number(bounds.maxX ?? 768000);
  const minY = Number(bounds.minY ?? -768000);
  const maxY = Number(bounds.maxY ?? 768000);
  let px = ((x - minX) / (maxX - minX)) * 100;
  let py = ((y - minY) / (maxY - minY)) * 100;
  if (bounds.invertX ?? true) px = 100 - px;
  if (bounds.invertY ?? true) py = 100 - py;
  if (px < -5 || px > 105 || py < -5 || py > 105) return '';
  const steamId = pick(obj, ['steamId', 'steam', 'userId'], '');
  const runtimeKey = pick(obj, ['runtimeKey'], '');
  const z = Number(pick(obj, ['z', 'Z', 'locationZ', 'LocationZ'], 0));
  const label = kind === 'player' ? `<span class="map-marker-label">${escapeHtml(title || pick(obj, ['name', 'Name'], 'игрок'))}</span>` : '';
  return `<span class="map-marker-wrap ${kind}" title="${escapeAttr(title)}" style="left:${px}%;top:${py}%"
    data-map-marker="1"
    data-kind="${escapeAttr(kind)}"
    data-title="${escapeAttr(title)}"
    data-steam="${escapeAttr(steamId)}"
    data-name="${escapeAttr(pick(obj, ['name', 'Name', 'ownerName'], title || ''))}"
    data-runtime="${escapeAttr(runtimeKey)}"
    data-x="${escapeAttr(rawX)}"
    data-y="${escapeAttr(rawY)}"
    data-z="${escapeAttr(Number.isFinite(z) ? z : 0)}">
      ${label}
      <button type="button" class="marker ${kind}" aria-label="${escapeAttr(title)}"></button>
    </span>`;
}

function setMapSelection(selection) {
  state.mapSelection = selection || null;
  renderMapSelection();
}

function renderMapSelection() {
  if (!els.mapSelection) return;
  const selected = state.mapSelection;
  if (!selected) {
    els.mapSelection.innerHTML = '<div>Ничего не выбрано.</div>';
    return;
  }
  const title = selected.title || selected.name || selected.kind || 'Точка';
  const x = Number(selected.x || 0);
  const y = Number(selected.y || 0);
  const z = Number(selected.z || 0);
  const canOpenPlayer = selected.kind === 'player' && (selected.steamId || selected.name);
  els.mapSelection.innerHTML = `
    <div><b>${escapeHtml(title)}</b></div>
    <div>${escapeHtml(selected.kind || 'точка')}</div>
    <div>X ${x.toFixed(0)} · Y ${y.toFixed(0)} · Z ${z.toFixed(0)}</div>
    <div class="button-row compact">
      <button class="btn" data-map-selection-action="use-coords">В телепорт</button>
      <button class="btn" data-map-selection-action="copy-coords">Копировать</button>
      ${canOpenPlayer ? '<button class="btn primary" data-map-selection-action="open-player">Открыть игрока</button>' : ''}
    </div>`;
}

function applySelectedMapCoords() {
  const selected = state.mapSelection;
  if (!selected) return;
  const x = Number(selected.x || 0).toFixed(0);
  const y = Number(selected.y || 0).toFixed(0);
  const z = Number(selected.z || 0).toFixed(0);
  if (els.teleportX) els.teleportX.value = x;
  if (els.teleportY) els.teleportY.value = y;
  if (els.teleportZ) els.teleportZ.value = z;
  if (els.playerActionX) els.playerActionX.value = x;
  if (els.playerActionY) els.playerActionY.value = y;
  if (els.playerActionZ) els.playerActionZ.value = z;
}

async function refreshEvents() {
  const [chatData, actionData] = await Promise.all([
    api('/api/chat?limit=80').catch(() => []),
    api('/api/action-log?limit=30').catch(() => [])
  ]);
  const chat = Array.isArray(chatData) ? chatData : (chatData.recent || []);
  const actions = Array.isArray(actionData) ? actionData : [];
  const list = chat.concat(actions).sort((a, b) => eventTime(b) - eventTime(a));
  els.recentEvents.innerHTML = renderEvents(list.slice(0, 8));
}

function eventTime(event) {
  const raw = event && (event.timestampUtc || event.utc || event.time || event.at || event.startedAt || event.expiresAt || event.createdAt);
  if (typeof raw === 'number') return raw * 1000;
  const dt = new Date(raw || 0);
  return Number.isNaN(dt.getTime()) ? 0 : dt.getTime();
}

function renderEvents(list) {
  return list.length ? list.map(e => {
    const actor = e.source === 'panel' && (e.targetName || e.targetSteamId)
      ? `Панель -> ${e.targetName || e.targetSteamId}`
      : (e.name || e.steamId || e.source || '');
    const kind = e.action || e.type || '';
    const stamp = e.timestampUtc || e.utc || '';
    const message = e.message || e.result || (e.ok === false ? 'не выполнено' : '');
    return `<article class="event">
    <div class="meta"><span>${escapeHtml(kind)}</span><span>${fmtTime(stamp)}</span></div>
    ${actor ? `<small>${escapeHtml(actor)}</small>` : ''}
    <div>${escapeHtml(message)}</div>
  </article>`;
  }).join('') : '<div class="event">Событий пока нет.</div>';
}

function eventMatches(event, query) {
  if (!query) return true;
  return JSON.stringify(event || {}).toLowerCase().includes(query.toLowerCase());
}

async function refreshSquads() {
  const data = await api('/api/squads');
  const payload = unwrapApiEnvelope(data);
  if (payload && typeof payload === 'object' && Array.isArray(payload.squads)) {
    state.squads = payload.squads;
    renderSquads();
    return;
  }
  const rows = Array.isArray(payload)
    ? payload
    : arrayFromPayload(payload || data, ['rows', 'members', 'squads']);
  const grouped = new Map();
  for (const row of rows) {
    const id = pick(row, ['squadId', 'id'], '');
    const key = id || pick(row, ['squadName', 'name'], 'squad');
    if (!grouped.has(key)) grouped.set(key, Object.assign({}, row, { members: [] }));
    const squad = grouped.get(key);
    const memberName = pick(row, ['memberName', 'name'], '');
    const memberSteam = pick(row, ['memberSteamId', 'steamId'], '');
    if (memberName || memberSteam) squad.members.push({
      name: memberName,
      steamId: memberSteam,
      memberId: pick(row, ['memberId'], ''),
      squadId: id,
      squadName: pick(row, ['squadName', 'name'], ''),
      rank: pick(row, ['memberRank', 'rank'], '')
    });
  }
  state.squads = Array.from(grouped.values());
  renderSquads();
}

function renderSquads() {
  const q = els.squadSearch ? els.squadSearch.value.trim() : '';
  const list = state.squads.filter(item => eventMatches(item, q));
  if (els.squadsList) els.squadsList.className = 'squad-list';
  const rankLabel = rank => {
    const value = Number(rank || 0);
    if (value >= 4) return 'Лидер';
    if (value === 3) return 'Заместитель';
    if (value === 2) return 'Офицер';
    if (value === 1) return 'Боец';
    return 'Участник';
  };
  const formatScore = value => {
    const number = Number(value || 0);
    return Number.isFinite(number) ? number.toLocaleString('ru-RU', { maximumFractionDigits: 1 }) : '-';
  };
  const squadKey = (item, index) => String(pick(item, ['squadId', 'id'], '') || pick(item, ['squadName', 'name', 'Name'], '') || `squad-${index}`);
  if (state.selectedSquadKey && !list.some((item, index) => squadKey(item, index) === state.selectedSquadKey)) {
    state.selectedSquadKey = '';
  }
  els.squadsList.innerHTML = list.length ? list.map((item, index) => {
    const members = (item.members || []).slice().sort((a, b) =>
      Number(b.rank || 0) - Number(a.rank || 0) ||
      String(a.name || a.steamId || '').localeCompare(String(b.name || b.steamId || ''))
    );
    const key = squadKey(item, index);
    const open = state.selectedSquadKey === key;
    const name = pick(item, ['squadName', 'name', 'Name'], 'Отряд');
    const leader = members.find(member => Number(member.rank || 0) >= 4) || members[0] || {};
    const memberLimit = pick(item, ['memberLimit'], '');
    const lastSeen = pick(item, ['lastMemberLoginUtc', 'lastMemberLogoutUtc'], '');
    const description = pick(item, ['information', 'message'], '');
    const memberRows = members.length ? members.map(member => `<div class="member-row ${Number(member.rank || 0) >= 4 ? 'leader' : ''}">
      <button class="member-main" type="button" data-squad-member="true" data-steam="${escapeAttr(member.steamId || '')}" data-name="${escapeAttr(member.name || '')}">
        <span class="member-name">${escapeHtml(member.name || member.steamId || 'Участник')}</span>
        <span class="member-rank">${escapeHtml(rankLabel(member.rank))}</span>
      </button>
      <button class="mini-action danger squad-kick-btn" type="button" data-squad-kick="true" data-member-id="${escapeAttr(member.memberId || '')}" data-squad-id="${escapeAttr(member.squadId || '')}" data-steam="${escapeAttr(member.steamId || '')}" data-name="${escapeAttr(member.name || '')}" data-squad-name="${escapeAttr(member.squadName || name || '')}">Исключить</button>
    </div>`).join('') : '<div class="member-row empty-member"><span class="member-name">Участники не найдены</span><span class="member-rank">-</span></div>';
    return `<article class="squad-list-card ${open ? 'is-open' : ''}">
      <button class="squad-summary-row" type="button" data-squad-open="${escapeAttr(key)}" aria-expanded="${open ? 'true' : 'false'}">
        <div class="squad-icon" aria-hidden="true">SQ</div>
        <div class="squad-title">
          <h3 class="squad-name">${escapeHtml(name)}</h3>
          <span class="squad-count">${members.length}${memberLimit ? ` / ${escapeHtml(memberLimit)}` : ''} участников</span>
        </div>
        <div class="squad-summary-stats">
          <span><b>${escapeHtml(formatScore(pick(item, ['score'], 0)))}</b><small>очки</small></span>
          <span><b>${escapeHtml(leader.name || leader.steamId || '-')}</b><small>лидер</small></span>
          <span><b>${escapeHtml(lastSeen ? fmtTime(lastSeen) : '-')}</b><small>активность</small></span>
        </div>
        <span class="squad-open-pill">${open ? 'Скрыть' : 'Открыть'}</span>
      </button>
      ${open ? `<div class="squad-detail-panel">
        ${description ? `<p class="squad-desc">${escapeHtml(description)}</p>` : '<p class="squad-desc muted-line">Описание отряда не задано.</p>'}
        <div class="squad-detail-grid">
          <span><b>${escapeHtml(formatScore(pick(item, ['score'], 0)))}</b><small>очки отряда</small></span>
          <span><b>${escapeHtml(leader.name || leader.steamId || '-')}</b><small>лидер</small></span>
          <span><b>${escapeHtml(members.length)}${memberLimit ? ` / ${escapeHtml(memberLimit)}` : ''}</b><small>состав</small></span>
        </div>
        <div class="squad-members-list">${memberRows}</div>
      </div>` : ''}
    </article>`;
  }).join('') : '<div class="empty-state">Данные по отрядам пока не найдены.</div>';
}

async function refreshChat() {
  const data = await api('/api/chat?limit=300');
  state.chat = arrayFromPayload(data, ['chat', 'messages', 'recent', 'events']);
  await refreshChatAuxChannel(els.chatChannel ? els.chatChannel.value : '', true).catch(() => {});
  renderChat();
}

async function refreshChatAuxChannel(channel, force = false) {
  if (channel !== 'killfeed' && channel !== 'nedjin') return;
  if (!force && state.chatAuxLoaded[channel]) return;
  if (state.chatAuxLoading[channel]) return state.chatAuxLoading[channel];
  state.chatAuxLoading[channel] = (async () => {
    if (channel === 'killfeed') {
      const data = await api('/api/kills');
      state.kills = normalizeKillEvents(arrayFromPayload(data, ['kills', 'events', 'recent', 'rows']));
    } else if (channel === 'nedjin') {
      const data = await api('/api/action-log?limit=300');
      state.actionLog = arrayFromPayload(data, ['actions', 'events', 'recent', 'rows']);
    }
    state.chatAuxLoaded[channel] = true;
  })().finally(() => {
    state.chatAuxLoading[channel] = null;
  });
  return state.chatAuxLoading[channel];
}

function renderChat() {
  const q = els.chatSearch ? els.chatSearch.value.trim() : '';
  const channel = els.chatChannel ? els.chatChannel.value : 'global';
  const list = dedupeChatMessages(chatRowsForChannel(channel).filter(item => {
    const itemChannel = normalizeChatChannel(item);
    return itemChannel === channel && eventMatches(item, q);
  })).sort((a, b) => eventTime(b) - eventTime(a));
  if (els.chatList) els.chatList.classList.add('chat-container');
  els.chatList.innerHTML = renderChatMessages(list);
}

function chatRowsForChannel(channel) {
  if (channel === 'killfeed') return collectKillEvents(false).map(killEventToChatRow);
  if (channel === 'nedjin') return (Array.isArray(state.actionLog) ? state.actionLog : []).map(nedjinEventToChatRow);
  return Array.isArray(state.chat) ? state.chat : [];
}

function fmtChatTime(event) {
  const raw = event && (event.timestampUtc || event.utc || event.time || event.at);
  if (!raw) return '';
  const dt = new Date(raw);
  if (Number.isNaN(dt.getTime())) return '';
  return dt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function chatActor(event) {
  if (!event) return 'Сервер';
  if (event.source === 'panel' && (event.targetName || event.targetSteamId)) {
    return repairMojibakeText(`Панель -> ${event.targetName || event.targetSteamId}`);
  }
  return repairMojibakeText(event.name || event.player || event.playerName || event.nickname || event.steamId || event.source || 'Сервер');
}

function chatSteamId(event) {
  return String(pick(event || {}, ['steamId', 'SteamId', 'steam', 'playerSteamId', 'sourceSteamId', 'targetSteamId'], '') || '').trim();
}

function chatText(event) {
  if (!event) return '';
  return repairMojibakeText(event.message || event.text || event.body || event.result || (event.ok === false ? 'не выполнено' : ''));
}

function killEventToChatRow(event) {
  const killer = cleanKillParticipantName(pick(event, ['killerName', 'killer', 'attackerName', 'attacker'], '') || killTextParticipant(event, 'killer')) || 'PVE';
  const victim = cleanKillParticipantName(pick(event, ['victimName', 'victim', 'targetName', 'target'], '') || killTextParticipant(event, 'victim')) || 'игрок';
  const weaponRaw = parseKillWeapon(event);
  const sourceLabel = killSourceLabel(event, killer, victim);
  const distance = parseKillDistance(event);
  const details = [
    victim,
    sourceLabel ? `${weaponRaw ? 'оружие' : 'причина'}: ${sourceLabel}` : '',
    distance !== '' && Number.isFinite(Number(distance)) ? `${Math.round(Number(distance))} м` : ''
  ].filter(Boolean).join(' • ');
  return Object.assign({}, event, {
    channel: 'killfeed',
    source: 'killfeed',
    name: killer,
    steamId: pick(event, ['killerSteamId', 'attackerSteamId'], '') || pick(event, ['victimSteamId', 'targetSteamId'], ''),
    message: details ? `убил ${details}` : `убил ${victim}`
  });
}

function nedjinEventToChatRow(event) {
  const action = pick(event, ['action', 'type', 'command'], 'nedjin');
  const steamId = pick(event, ['steamId', 'SteamId', 'targetSteamId', 'targetSteam', 'playerSteamId'], '');
  const actor = event && event.source === 'panel' && (event.targetName || event.targetSteamId)
    ? `Панель -> ${event.targetName || event.targetSteamId}`
    : pick(event, ['name', 'player', 'playerName', 'targetName', 'target', 'source'], 'NeDjin');
  const message = pick(event, ['message', 'result', 'text', 'body'], '') || actionTextBlob(event) || action;
  return Object.assign({}, event, {
    channel: 'nedjin',
    source: 'nedjin',
    name: `${action}`,
    steamId,
    targetName: actor,
    message
  });
}

function chatDedupeKey(event) {
  const time = eventTime(event);
  const second = time ? Math.floor(time / 1000) : '';
  return [
    second,
    normalizeChatChannel(event),
    String(chatSteamId(event) || '').trim(),
    String(chatActor(event) || '').trim().toLowerCase(),
    String(chatText(event) || '').replace(/\s+/g, ' ').trim().toLowerCase()
  ].join('|');
}

function dedupeChatMessages(list) {
  const seen = new Set();
  const out = [];
  for (const event of list || []) {
    const key = chatDedupeKey(event);
    if (!key || seen.has(key)) continue;
    seen.add(key);
    out.push(event);
  }
  return out;
}

function normalizeChatChannel(event) {
  const rawChannel = String(event && (event.channel || event.chatChannel || event.kind || '') || '').trim().toLowerCase();
  const source = String(event && (event.source || event.path || event.type || '') || '').toLowerCase();
  const text = String(chatText(event) || '').trim();
  const lowerText = text.toLowerCase();
  const numericChannels = {
    '0': 'local',
    '1': 'squad',
    '2': 'global',
    '3': 'admin',
    '4': 'command'
  };
  if (numericChannels[rawChannel]) return numericChannels[rawChannel];
  if (
    rawChannel.includes('kill') ||
    rawChannel.includes('combat') ||
    source.includes('kill-feed') ||
    source.includes('killfeed') ||
    source.includes('savefile-kill-log') ||
    String(event && event.type || '').toLowerCase() === 'kill'
  ) return 'killfeed';
  if (
    rawChannel.includes('nedjin') ||
    source.includes('nedjin') ||
    source.includes('panel') ||
    source.includes('action-log') ||
    source.includes('bridge')
  ) return 'nedjin';
  if (
    rawChannel.includes('command') ||
    source.includes('processadmincommand') ||
    source.includes('command-reply') ||
    text.startsWith('/') ||
    text.startsWith('#')
  ) return 'command';
  if (rawChannel.includes('local') || rawChannel.includes('proximity') || rawChannel.includes('vicinity') || rawChannel.includes('лок')) return 'local';
  if (rawChannel.includes('squad') || rawChannel.includes('team') || rawChannel.includes('clan') || rawChannel.includes('party') || rawChannel.includes('отряд')) return 'squad';
  if (rawChannel.includes('admin') || rawChannel.includes('moderator') || rawChannel.includes('админ')) return 'admin';
  if (rawChannel.includes('global') || rawChannel.includes('world') || rawChannel.includes('глоб')) return 'global';
  if (rawChannel === 'server' && (lowerText.startsWith('/') || source.includes('nedjin') || source.includes('command'))) return 'command';
  if (rawChannel === 'server') return 'server';
  if (rawChannel === 'chat' || !rawChannel) return 'global';
  return rawChannel;
}

function chatChannelLabel(channel) {
  const labels = {
    global: 'Глобальный',
    local: 'Локальный',
    squad: 'Отряд',
    admin: 'Админ',
    command: 'Команды',
    killfeed: 'Killfeed',
    nedjin: 'NeDjin'
  };
  return labels[channel] || channel || 'Чат';
}

function chatChannelBadge(channel) {
  const labels = {
    global: 'Global',
    local: 'Local',
    squad: 'Squad',
    admin: 'Admin',
    command: 'Command',
    killfeed: 'Killfeed',
    nedjin: 'NeDjin',
    server: 'Server'
  };
  return labels[channel] || String(channel || 'Chat');
}

function renderChatMessages(list) {
  if (!list.length) return '<div class="empty-state">Сообщений пока нет.</div>';
  return list.map(event => {
    const channel = normalizeChatChannel(event);
    const actor = chatActor(event);
    const steamId = chatSteamId(event);
    const canOpenProfile = Boolean(steamId || (actor && channel !== 'killfeed' && channel !== 'nedjin'));
    const actions = steamId || canOpenProfile
      ? `<div class="chat-actions">
          ${steamId ? `<button class="chat-action-btn" data-chat-act="copy-id" data-steam="${escapeAttr(steamId)}" title="Копировать SteamID">ID</button>` : ''}
          ${canOpenProfile ? `<button class="chat-action-btn" data-chat-act="profile" data-steam="${escapeAttr(steamId)}" data-name="${escapeAttr(actor)}" title="Открыть профиль">Профиль</button>` : ''}
        </div>`
      : '';
    return `<article class="chat-message chat-msg chat-msg-${escapeAttr(channel)}" data-steam="${escapeAttr(steamId)}" data-name="${escapeAttr(actor)}">
      <time class="chat-time">${escapeHtml(fmtChatTime(event))}</time>
      <span class="chat-badge ${escapeAttr(channel)}" title="${escapeAttr(chatChannelLabel(channel))}">${escapeHtml(chatChannelBadge(channel))}</span>
      <button class="chat-author" data-chat-act="profile" data-steam="${escapeAttr(steamId)}" data-name="${escapeAttr(actor)}" title="${escapeAttr(steamId ? `SteamID: ${steamId}` : 'Открыть профиль')}">${escapeHtml(actor)}</button>
      <span class="chat-text">${escapeHtml(chatText(event))}</span>
      ${actions}
    </article>`;
  }).join('');
}

async function refreshKills() {
  const data = await api('/api/kills');
  state.kills = normalizeKillEvents(arrayFromPayload(data, ['kills', 'events', 'recent', 'rows']));
  renderKills();
}

function renderKills() {
  const q = els.killsSearch ? els.killsSearch.value.trim() : '';
  const list = collectKillEvents(false)
    .filter(item => eventMatches(item, q))
    .sort((a, b) => eventTime(b) - eventTime(a));
  if (els.killsList) {
    els.killsList.classList.remove('kill-log-container');
    els.killsList.classList.add('killfeed-container');
  }
  if (els.killsList) els.killsList.innerHTML = renderKillEvents(list);
}

function logClearTargetForChatChannel(channel) {
  if (channel === 'killfeed') return 'kills';
  if (channel === 'nedjin') return 'action-log';
  return 'chat';
}

async function clearProjectLogs(target) {
  const cleanTarget = String(target || 'chat').trim() || 'chat';
  return api('/api/logs/clear', {
    method: 'POST',
    body: JSON.stringify({ target: cleanTarget })
  });
}

function resetLocalLogState(target) {
  if (target === 'kills' || target === 'killfeed') {
    state.kills = [];
    state.chatAuxLoaded.killfeed = true;
    return;
  }
  if (target === 'action-log' || target === 'nedjin') {
    state.actionLog = [];
    state.chatAuxLoaded.nedjin = true;
    return;
  }
  state.chat = [];
}

function killEventTextBlob(event) {
  if (!event || typeof event !== 'object') return '';
  const chunks = [];
  Object.keys(event).forEach(key => {
    const value = event[key];
    if (value == null) return;
    if (typeof value === 'string' || typeof value === 'number') {
      chunks.push(String(value));
    } else if (typeof value === 'object' && key.toLowerCase().includes('raw')) {
      try { chunks.push(JSON.stringify(value)); } catch (_) {}
    }
  });
  return chunks.join(' ');
}

function killRawText(event) {
  if (!event || typeof event !== 'object') return '';
  const raw = event.raw ?? event.Raw ?? event.rawText ?? event.logLine ?? event.messageRaw ?? '';
  const text = String(raw || '').trim();
  return text || killEventTextBlob(event);
}

function killLikeText(event) {
  return `${killEventTextBlob(event)} ${actionTextBlob(event)}`.replace(/\s+/g, ' ').trim();
}

function isKillNoiseEvent(event) {
  const text = killLikeText(event).toLowerCase();
  return /updating profile deletion|profile deletion because prisoner died|game version:|log file open/i.test(text);
}

function isKillLikeEvent(event) {
  if (!event || typeof event !== 'object') return false;
  if (isKillNoiseEvent(event)) return false;
  const type = String(event.type || event.action || '').toLowerCase();
  if (type.includes('kill') || type.includes('death')) return true;
  const text = killLikeText(event);
  return /was killed by|died\s*:|killer\s*:|victimloc|killerloc|убил|погиб|смерт|kill feed/i.test(text);
}

function textQuality(value) {
  const text = String(value || '').trim();
  if (!text) return -100;
  let score = Math.min(text.length, 32);
  if (/[A-Za-zА-Яа-яЁё0-9]/.test(text)) score += 20;
  if (/[\u0000-\u001f\u007f-\u009f�]/.test(text)) score -= 80;
  if (/^[\\u0-9;:@<>{}\s]+$/.test(text)) score -= 40;
  return score;
}

function killEventQuality(event) {
  let score = 0;
  if (parseKillWeapon(event)) score += 10;
  if (parseKillDistance(event) !== '') score += 8;
  if (parseKillLocation(event, 'killer')) score += 4;
  if (parseKillLocation(event, 'victim')) score += 4;
  const source = String(event && (event.source || event.logFile || '') || '').toLowerCase();
  if (source.includes('savefile') || source.includes('kill_')) score += 8;
  if (String(event && event.raw || '').includes('Weapon:')) score += 8;
  return score;
}

function killHasDisplayIdentity(event) {
  const victim = killParticipantIdentity(event, 'victim');
  const killer = killParticipantIdentity(event, 'killer');
  const victimText = cleanKillParticipantName(valueFromKeys(event, ['victimName', 'victim', 'targetName', 'target']) || killTextParticipant(event, 'victim'));
  const killerText = cleanKillParticipantName(valueFromKeys(event, ['killerName', 'killer', 'attackerName', 'attacker', 'sourceName']) || killTextParticipant(event, 'killer'));
  const hasVictim = victim.ids.length > 0 || !!victim.name || !!normalizedKillIdentity(victimText);
  const hasKiller = killer.ids.length > 0 || !!killer.name || !!normalizedKillIdentity(killerText);
  if (!hasVictim) return false;
  if (hasKiller) return true;
  const text = killLikeText(event).toLowerCase();
  return /погиб|died|was killed by|victimloc|killerloc|weapon\s*:/i.test(text);
}

function isDisplayableKillEvent(event) {
  if (!isKillLikeEvent(event)) return false;
  if (!killHasDisplayIdentity(event)) return false;
  const killer = normalizedKillIdentity(valueFromKeys(event, ['killerName', 'killer', 'attackerName', 'attacker', 'sourceName']) || killTextParticipant(event, 'killer'));
  const victim = normalizedKillIdentity(valueFromKeys(event, ['victimName', 'victim', 'targetName', 'target']) || killTextParticipant(event, 'victim'));
  if (!killer && !victim && killEventQuality(event) < 20) return false;
  return true;
}

function isTransientServerKillEvent(event) {
  const source = String(event && event.source || '').toLowerCase();
  const logFile = String(event && event.logFile || '').toLowerCase();
  const raw = killRawText(event);
  if (!source.includes('server-log') || logFile !== 'scum.log') return false;
  if (!/was killed by/i.test(raw)) return false;
  return !parseKillWeapon(event) &&
    parseKillDistance(event) === '' &&
    !parseKillLocation(event, 'killer') &&
    !parseKillLocation(event, 'victim');
}

function betterText(current, next) {
  return textQuality(next) > textQuality(current) ? next : current;
}

function killEventGroupKey(event) {
  const time = eventTime(event);
  const second = time ? Math.round(time / 1000) : '';
  const victimId = String(valueFromKeys(event, ['victimSteamId', 'targetSteamId', 'victimUserProfileId', 'victimProfileId']) || '').trim();
  const victimName = String(valueFromKeys(event, ['victimName', 'victim', 'targetName', 'target']) || killTextParticipant(event, 'victim') || '').trim().toLowerCase();
  const raw = String(event && event.raw || event && event.message || '').slice(0, 120);
  return `${second}|${victimId || victimName || raw}`;
}

function normalizedKillIdentity(value) {
  const text = cleanKillParticipantName(value).toLowerCase().replace(/\s+/g, ' ').trim();
  return text && !/^(unknown|неизвестно|npc|pve|-)$/.test(text) ? text : '';
}

function killParticipantIdentity(event, role) {
  const killer = role === 'killer';
  const idKeys = killer
    ? ['killerSteamId', 'attackerSteamId', 'killerUserId', 'attackerUserId', 'killerUserProfileId', 'killerProfileId', 'killerDbId']
    : ['victimSteamId', 'targetSteamId', 'victimUserId', 'targetUserId', 'victimUserProfileId', 'victimProfileId', 'victimDbId'];
  const nameKeys = killer
    ? ['killerName', 'killer', 'attackerName', 'attacker', 'sourceName']
    : ['victimName', 'victim', 'targetName', 'target'];
  const ids = idKeys
    .map(key => String(valueFromKeys(event, [key]) || '').trim())
    .filter(value => value && !/^(npc|unknown|none|null|0)$/i.test(value));
  const name = normalizedKillIdentity(valueFromKeys(event, nameKeys) || killTextParticipant(event, role));
  return { ids: [...new Set(ids)], name };
}

function killIdentitiesMatch(a, b, role) {
  const left = killParticipantIdentity(a, role);
  const right = killParticipantIdentity(b, role);
  if (left.ids.length && right.ids.length && left.ids.some(id => right.ids.includes(id))) return true;
  if (left.name && right.name && left.name === right.name) return true;
  return false;
}

function killHasParticipant(event, role) {
  const identity = killParticipantIdentity(event, role);
  return identity.ids.length > 0 || !!identity.name;
}

function killIsSelfOrDeath(event) {
  const type = String(event && (event.type || event.action || '') || '').toLowerCase();
  if (type.includes('death')) return true;
  return killIdentitiesMatch(event, event, 'killer') && killIdentitiesMatch(event, event, 'victim') &&
    killParticipantIdentity(event, 'killer').ids.some(id => killParticipantIdentity(event, 'victim').ids.includes(id));
}

function killTimestampLooksDuplicate(left, right) {
  const a = eventTime(left);
  const b = eventTime(right);
  if (!a || !b) return false;
  const delta = Math.abs(a - b);
  const moscowOffset = 3 * 60 * 60 * 1000;
  return delta <= 30 * 1000 || Math.abs(delta - moscowOffset) <= 5 * 60 * 1000;
}

function killParticipantsLookSame(left, right) {
  if (!killIdentitiesMatch(left, right, 'victim')) return false;
  if (killIdentitiesMatch(left, right, 'killer')) return true;
  const leftHasKiller = killHasParticipant(left, 'killer');
  const rightHasKiller = killHasParticipant(right, 'killer');
  if (!leftHasKiller || !rightHasKiller) return true;
  return killIsSelfOrDeath(left) || killIsSelfOrDeath(right);
}

function killEventsLikelyDuplicate(left, right) {
  if (!left || !right) return false;
  if (!killTimestampLooksDuplicate(left, right)) return false;
  if (!killParticipantsLookSame(left, right)) return false;
  const leftQuality = killEventQuality(left);
  const rightQuality = killEventQuality(right);
  const sourceText = `${left.source || ''} ${right.source || ''} ${left.logFile || ''} ${right.logFile || ''}`.toLowerCase();
  return leftQuality !== rightQuality || sourceText.includes('server-log') || sourceText.includes('savefile') || sourceText.includes('kill_');
}

function mergeKillEvents(left, right) {
  if (!left) return Object.assign({}, right);
  const merged = Object.assign({}, left);
  const preferRight = killEventQuality(right) > killEventQuality(left);
  for (const key of ['weapon', 'weaponRaw', 'cause', 'deathType', 'distance', 'distanceMeters', 'killerLocation', 'victimLocation', 'raw', 'logFile']) {
    if (right && right[key] != null && right[key] !== '' && (!merged[key] || preferRight)) merged[key] = right[key];
  }
  for (const key of ['killerName', 'killer', 'attackerName', 'attacker', 'victimName', 'victim', 'targetName', 'target']) {
    const chosen = betterText(merged[key], right && right[key]);
    if (chosen) merged[key] = chosen;
  }
  for (const key of ['killerSteamId', 'attackerSteamId', 'victimSteamId', 'targetSteamId', 'killerUserProfileId', 'victimUserProfileId']) {
    if ((!merged[key] || String(merged[key]).trim() === '') && right && right[key]) merged[key] = right[key];
  }
  if (right && right.message && (!merged.message || preferRight)) merged.message = right.message;
  if (right && right.broadcastMessage && (!merged.broadcastMessage || preferRight)) merged.broadcastMessage = right.broadcastMessage;
  if (right && right.source && !String(merged.source || '').includes(right.source)) merged.source = [merged.source, right.source].filter(Boolean).join(' + ');
  return merged;
}

function normalizeKillEvents(rows) {
  const knownNames = collectKillKnownNames(rows);
  const grouped = new Map();
  for (const row of rows || []) {
    const event = repairKillEventParticipants(hydrateKillEvent(parseMaybeJson(row)), knownNames);
    if (!isDisplayableKillEvent(event)) continue;
    const key = killEventGroupKey(event);
    grouped.set(key, mergeKillEvents(grouped.get(key), event));
  }
  const events = [...grouped.values()]
    .sort((a, b) => eventTime(b) - eventTime(a))
    .reduce((merged, event) => {
      const index = merged.findIndex(existing => killEventsLikelyDuplicate(existing, event));
      if (index >= 0) merged[index] = mergeKillEvents(merged[index], event);
      else merged.push(event);
      return merged;
    }, []);
  const useful = events.filter(event => {
    if (isTransientServerKillEvent(event)) return false;
    if (killEventQuality(event) >= 20) return true;
    return !events.some(other => {
      if (other === event || killEventQuality(other) < 20) return false;
      return killEventsLikelyDuplicate(event, other);
    });
  });
  return useful.sort((a, b) => eventTime(b) - eventTime(a));
}

function collectKillEvents(includeFallback = false) {
  const rows = Array.isArray(state.kills) ? state.kills.slice() : [];
  if (includeFallback) {
    rows.push(...(Array.isArray(state.chat) ? state.chat.filter(isDisplayableKillEvent) : []));
    rows.push(...(Array.isArray(state.actionLog) ? state.actionLog.filter(isDisplayableKillEvent) : []));
  }
  return normalizeKillEvents(rows);
}

function normalizeKillDistanceMeters(value) {
  const number = Number(String(value ?? '').replace(',', '.').replace(/[^\d.-]/g, ''));
  if (!Number.isFinite(number)) return '';
  return number > 5000 ? number / 100 : number;
}

function cleanKillParticipantName(value) {
  let text = repairMojibakeText(String(value || '')).trim();
  if (!text) return '';
  text = text.replace(/[\u0000-\u001f\u007f-\u009f]/g, '').trim();
  text = text.replace(/^['"]+|['"]+$/g, '').trim();
  const steamWrapped = text.match(/^\d{16,20}\s*:\s*(.+?)(?:\(\d+\))?$/);
  if (steamWrapped) text = steamWrapped[1].trim();
  text = text.replace(/\s*\(\d+\)\s*$/g, '').trim();
  text = text.replace(/^\[[^\]]+\]\s*/g, '').trim();
  if (/^(?:BP|BPC|NPC)_/i.test(text)) {
    text = cleanAssetId(text)
      .replace(/^(?:BP|BPC|NPC)_/i, '')
      .replace(/_/g, ' ')
      .trim();
  }
  return text;
}

function addKillKnownName(map, steam, name) {
  const id = String(steam || '').trim();
  const cleanName = cleanKillParticipantName(name);
  if (!/^\d{16,20}$/.test(id) || !cleanName) return;
  if (/^(unknown|неизвестно|npc|pve|user#\d+)$/i.test(cleanName)) return;
  const current = map.get(id) || '';
  if (textQuality(cleanName) > textQuality(current)) map.set(id, cleanName);
}

function collectKillKnownNames(rows) {
  const map = new Map();
  const addObject = obj => {
    if (!obj || typeof obj !== 'object') return;
    addKillKnownName(map, valueFromKeys(obj, ['killerSteamId', 'attackerSteamId', 'killerUserId', 'attackerUserId']), valueFromKeys(obj, ['killerName', 'killer', 'attackerName', 'attacker']));
    addKillKnownName(map, valueFromKeys(obj, ['victimSteamId', 'targetSteamId', 'victimUserId', 'targetUserId']), valueFromKeys(obj, ['victimName', 'victim', 'targetName', 'target']));
    const raw = killRawText(obj);
    const detailed = raw.match(/Died:\s*(.*?)\s*\((\d{16,20})\),\s*Killer:\s*(.*?)\s*\((\d{16,20})\)/i);
    if (detailed) {
      addKillKnownName(map, detailed[2], detailed[1]);
      addKillKnownName(map, detailed[4], detailed[3]);
    }
    raw.replace(/(\d{16,20})\s*:\s*([^'"\r\n()]+)\((\d+)\)/g, (_, steam, name) => {
      addKillKnownName(map, steam, name);
      return _;
    });
  };
  (Array.isArray(state.players) ? state.players : []).forEach(player => {
    addKillKnownName(map, valueFromKeys(player, ['steamId', 'steam', 'SteamId']), valueFromKeys(player, ['name', 'playerName', 'characterName', 'Name']));
  });
  (rows || []).forEach(row => addObject(parseMaybeJson(row)));
  return map;
}

function repairKillEventParticipants(event, knownNames) {
  if (!event || typeof event !== 'object') return event;
  const fixed = Object.assign({}, event);
  const repair = (role, steamKeys, nameKeys) => {
    const steam = String(valueFromKeys(fixed, steamKeys) || '').trim();
    if (!/^\d{16,20}$/.test(steam)) return;
    const known = knownNames && knownNames.get ? knownNames.get(steam) : '';
    if (!known) return;
    const current = valueFromKeys(fixed, nameKeys) || '';
    if (textQuality(known) <= textQuality(current)) return;
    fixed[nameKeys[0]] = known;
    if (role === 'killer' && fixed.killer) fixed.killer = known;
    if (role === 'victim' && fixed.victim) fixed.victim = known;
  };
  repair('killer', ['killerSteamId', 'attackerSteamId', 'killerUserId', 'attackerUserId'], ['killerName', 'killer', 'attackerName', 'attacker']);
  repair('victim', ['victimSteamId', 'targetSteamId', 'victimUserId', 'targetUserId'], ['victimName', 'victim', 'targetName', 'target']);
  return fixed;
}

function parseScumTimestampUtc(text) {
  const match = String(text || '').match(/(\d{4})\.(\d{2})\.(\d{2})-(\d{2})\.(\d{2})\.(\d{2})/);
  if (!match) return '';
  return `${match[1]}-${match[2]}-${match[3]}T${match[4]}:${match[5]}:${match[6]}Z`;
}

function scumLogLocation(value) {
  if (!value || typeof value !== 'object') return null;
  const x = Number(value.X ?? value.x);
  const y = Number(value.Y ?? value.y);
  const z = Number(value.Z ?? value.z);
  return [x, y, z].every(Number.isFinite) ? { x, y, z } : null;
}

function scumLogDistanceMeters(killerLocation, victimLocation) {
  if (!killerLocation || !victimLocation) return '';
  const dx = Number(killerLocation.x) - Number(victimLocation.x);
  const dy = Number(killerLocation.y) - Number(victimLocation.y);
  if (!Number.isFinite(dx) || !Number.isFinite(dy)) return '';
  return Math.sqrt(dx * dx + dy * dy) / 100;
}

function cleanScumWeapon(value) {
  const raw = String(value || '').trim();
  const withoutCause = raw.replace(/\[[^\]]+\]/g, '').trim();
  const cleaned = cleanAssetId(withoutCause.replace(/_C$/i, '')) || cleanAssetId(withoutCause) || withoutCause;
  return cleaned || '';
}

function parseScumKillJsonText(raw, event) {
  const source = String(raw || '');
  const start = source.indexOf('{"Killer"');
  if (start < 0) return null;
  const end = findJsonObjectEnd(source, start);
  if (end < 0) return null;
  let obj = null;
  try {
    obj = JSON.parse(source.slice(start, end + 1));
  } catch (_) {
    return null;
  }
  if (!obj || !obj.Killer || !obj.Victim) return null;
  const killer = obj.Killer || {};
  const victim = obj.Victim || {};
  const killerLocation = scumLogLocation(killer.ServerLocation);
  const victimLocation = scumLogLocation(victim.ServerLocation);
  const weaponRaw = String(obj.Weapon || '');
  const cause = (weaponRaw.match(/\[([^\]]+)\]/) || [])[1] || '';
  const weapon = cleanScumWeapon(weaponRaw);
  const killerSteam = /^\d{16,20}$/.test(String(killer.UserId || '').trim()) ? String(killer.UserId).trim() : '';
  const victimSteam = /^\d{16,20}$/.test(String(victim.UserId || '').trim()) ? String(victim.UserId).trim() : '';
  const killerName = cleanKillParticipantName(killer.ProfileName || killer.UserId || '');
  const victimName = cleanKillParticipantName(victim.ProfileName || victim.UserId || '');
  const distance = scumLogDistanceMeters(killerLocation, victimLocation);
  if (!victimName) return null;
  return {
    timestampUtc: parseScumTimestampUtc(source) || event.timestampUtc,
    message: killerSteam && victimSteam && killerSteam === victimSteam
      ? `${victimName} погиб`
      : `${killerName || 'NPC'} убил ${victimName}`,
    killerName,
    killerSteamId: killerSteam,
    victimName,
    victimSteamId: victimSteam,
    weapon,
    weaponRaw,
    cause,
    deathType: killCauseLabel(cause),
    distance,
    distanceMeters: distance,
    killerLocation,
    victimLocation,
    source: 'savefile-kill-log',
    raw: source
  };
}

function parseScumKillLogText(event) {
  const raw = killRawText(event);
  if (!raw) return null;
  const parsedJson = parseScumKillJsonText(raw, event || {});
  if (parsedJson) return parsedJson;
  const detailed = raw.match(/Died:\s*(.*?)\s*\(([^)]*)\),\s*Killer:\s*(.*?)\s*\(([^)]*)\)\s*Weapon:\s*([\s\S]*?)\s+S:?\[?\s*KillerLoc\s*:?\s*(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)\s+VictimLoc\s*:?\s*(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)\s*,?\s*Distance\s*:\s*([0-9]+(?:[.,][0-9]+)?)\s*m/i);
  if (detailed) {
    const weaponRaw = detailed[5].trim();
    const cause = (weaponRaw.match(/\[([^\]]+)\]/) || [])[1] || '';
    const weapon = cleanScumWeapon(weaponRaw);
    const number = value => Number(String(value || '').replace(',', '.'));
    const killerSteam = /^\d{16,20}$/.test(detailed[4].trim()) ? detailed[4].trim() : '';
    const victimSteam = /^\d{16,20}$/.test(detailed[2].trim()) ? detailed[2].trim() : '';
    const killerName = cleanKillParticipantName(detailed[3]);
    const victimName = cleanKillParticipantName(detailed[1]);
    return {
      timestampUtc: parseScumTimestampUtc(raw) || event.timestampUtc,
      message: killerSteam && victimSteam && killerSteam === victimSteam
        ? `${victimName} погиб`
        : `${killerName || 'NPC'} убил ${victimName}`,
      killerName,
      killerSteamId: killerSteam,
      victimName,
      victimSteamId: victimSteam,
      weapon,
      weaponRaw,
      cause,
      deathType: killCauseLabel(cause),
      distance: normalizeKillDistanceMeters(detailed[12]),
      distanceMeters: normalizeKillDistanceMeters(detailed[12]),
      killerLocation: { x: number(detailed[6]), y: number(detailed[7]), z: number(detailed[8]) },
      victimLocation: { x: number(detailed[9]), y: number(detailed[10]), z: number(detailed[11]) },
      source: 'savefile-kill-log',
      raw
    };
  }

  const short = raw.match(/['"]([^'"]+)['"]\s+was killed by\s+['"]([^'"]+)['"]/i);
  if (short) {
    const victimRaw = short[1].trim();
    const killerRaw = short[2].trim();
    const victimSteam = (victimRaw.match(/^(\d{16,20})\s*:/) || [])[1] || '';
    const killerSteam = (killerRaw.match(/^(\d{16,20})\s*:/) || [])[1] || '';
    const victimName = cleanKillParticipantName(victimRaw);
    const killerName = cleanKillParticipantName(killerRaw);
    return {
      timestampUtc: parseScumTimestampUtc(raw) || event.timestampUtc,
      message: killerSteam && victimSteam && killerSteam === victimSteam
        ? `${victimName} погиб`
        : `${killerName} убил ${victimName}`,
      killerName,
      killerSteamId: killerSteam,
      victimName,
      victimSteamId: victimSteam,
      source: event.source || 'server-log',
      raw
    };
  }
  return null;
}

function hydrateKillEvent(event) {
  if (!event || typeof event !== 'object') return event;
  const parsed = parseScumKillLogText(event);
  return parsed ? Object.assign({}, event, parsed) : event;
}

function parseKillDistance(event) {
  const direct = pick(event, ['distanceMeters', 'distanceM', 'distance', 'Distance'], '');
  if (direct !== '') {
    return normalizeKillDistanceMeters(direct);
  }
  const raw = killEventTextBlob(event);
  const match = raw.match(/Distance\s*:\s*([0-9]+(?:[.,][0-9]+)?)\s*m/i) || raw.match(/([0-9]+(?:[.,][0-9]+)?)\s*m\b/i);
  if (!match) return '';
  return normalizeKillDistanceMeters(match[1]);
}

function parseKillWeapon(event) {
  const direct = pick(event, ['weapon', 'weaponName', 'weaponRaw', 'item', 'Weapon'], '');
  if (direct) return cleanScumWeapon(direct) || cleanAssetId(direct) || direct;
  const raw = killEventTextBlob(event);
  const match = raw.match(/Weapon\s*:\s*([^,\]|]+)/i) || raw.match(/\b(Weapon_[A-Za-z0-9_]+)\b/);
  return match ? cleanScumWeapon(match[1].trim()) : '';
}

function killCauseLabel(value) {
  const text = String(value || '').trim();
  const lower = text.toLowerCase();
  if (lower === 'projectile') return 'пуля/снаряд';
  if (lower === 'melee') return 'ближний бой';
  if (lower === 'explosion') return 'взрыв';
  if (lower === 'vehicle') return 'транспорт';
  if (lower === 'fall') return 'падение';
  if (lower === 'suicide') return 'самоубийство';
  if (lower === 'zombie' || lower === 'puppet') return 'зомби';
  if (lower === 'sentry') return 'сентри';
  if (lower === 'animal') return 'животное';
  if (lower === 'npc' || lower === 'pve' || lower === 'monster' || lower === 'drifter') return 'NPC/монстр';
  return text;
}

function parseKillCause(event) {
  const directValues = ['deathType', 'cause', 'damageType', 'kindOfDeath', 'DeathType']
    .map(key => pick(event, [key], ''))
    .filter(value => String(value || '').trim() !== '');
  for (const value of directValues) {
    const label = killCauseLabel(value);
    if (label && !/^[?\s/\\-]+$/.test(label) && !String(label).includes('????')) return label;
  }
  const raw = killEventTextBlob(event);
  const bracket = raw.match(/\[([A-Za-z_ -]+)\]/);
  if (bracket) {
    return killCauseLabel(bracket[1]);
  }
  if (/zombie|puppet/i.test(raw)) return 'зомби';
  if (/sentry/i.test(raw)) return 'сентри';
  if (/animal|bear|wolf|boar|goat|horse|donkey|chicken/i.test(raw)) return 'животное';
  if (/npc|drifter|monster/i.test(raw)) return 'NPC/монстр';
  return 'неизвестно';
}

function killSourceLabel(event, killer = '', victim = '') {
  const weaponRaw = parseKillWeapon(event);
  if (weaponRaw) return friendlyAssetName(weaponRaw, 'item');

  const cause = parseKillCause(event);
  const blob = `${killEventTextBlob(event)} ${killer} ${victim}`.toLowerCase();
  if (/zombie|puppet/.test(blob)) return 'Зомби';
  if (/sentry/.test(blob)) return 'Сентри';
  if (/animal|bear|wolf|boar|goat|horse|donkey|chicken/.test(blob)) return 'Животное';
  if (/npc|drifter|monster/.test(blob)) return 'NPC/монстр';

  const kind = killKind(event, killer, victim);
  if (kind === 'self' || /suicide/.test(blob) || String(cause || '').toLowerCase().includes('самоубий')) {
    return 'Суицид';
  }
  if (cause && cause !== 'неизвестно') return cause;
  if (kind === 'pve') return 'NPC/PVE';
  return 'Без оружия';
}

function parseLocationValue(value) {
  if (!value) return null;
  if (typeof value === 'object') {
    const x = Number(value.x ?? value.X ?? value[0]);
    const y = Number(value.y ?? value.Y ?? value[1]);
    const z = Number(value.z ?? value.Z ?? value[2]);
    return [x, y, z].every(Number.isFinite) ? { x, y, z } : null;
  }
  const match = String(value).match(/(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)/);
  if (!match) return null;
  const nums = match.slice(1, 4).map(part => Number(String(part).replace(',', '.')));
  return nums.every(Number.isFinite) ? { x: nums[0], y: nums[1], z: nums[2] } : null;
}

function parseKillLocation(event, role) {
  const prefix = role === 'killer' ? 'killer' : 'victim';
  const direct = pick(event, [
    `${prefix}Location`,
    `${prefix}Loc`,
    `${prefix}Position`,
    `${prefix}Coords`,
    `${prefix}Coordinates`
  ], '');
  const directLocation = parseLocationValue(direct);
  if (directLocation) return directLocation;

  const x = pick(event, [`${prefix}X`, `${prefix}LocX`, `${prefix}LocationX`, `${prefix}_x`], '');
  const y = pick(event, [`${prefix}Y`, `${prefix}LocY`, `${prefix}LocationY`, `${prefix}_y`], '');
  const z = pick(event, [`${prefix}Z`, `${prefix}LocZ`, `${prefix}LocationZ`, `${prefix}_z`], '');
  const numeric = [x, y, z].map(part => Number(String(part).replace(',', '.')));
  if (numeric.every(Number.isFinite)) return { x: numeric[0], y: numeric[1], z: numeric[2] };

  const label = role === 'killer' ? 'Killer' : 'Victim';
  const raw = killEventTextBlob(event);
  const re = new RegExp(`${label}(?:Loc|Location)?\\s*:?\\s*(-?\\d+(?:[.,]\\d+)?)\\s*,\\s*(-?\\d+(?:[.,]\\d+)?)\\s*,\\s*(-?\\d+(?:[.,]\\d+)?)`, 'i');
  const match = raw.match(re);
  return match ? parseLocationValue(match.slice(1, 4).join(', ')) : null;
}

function fmtKillLocation(location) {
  if (!location) return '-';
  const x = Number(location.x);
  const y = Number(location.y);
  const z = Number(location.z);
  if (![x, y, z].every(Number.isFinite)) return '-';
  if (Math.round(x) === 0 && Math.round(y) === 0 && Math.round(z) === 0) return '-';
  return `${Math.round(location.x)}, ${Math.round(location.y)}, ${Math.round(location.z)}`;
}

function killKind(event, killer, victim) {
  const type = String(pick(event, ['kind', 'type', 'source'], '')).toLowerCase();
  const killerSteam = String(pick(event, ['killerSteamId', 'attackerSteamId', 'sourceSteamId'], '')).trim();
  const victimSteam = String(pick(event, ['victimSteamId', 'targetSteamId'], '')).trim();
  const sameSteam = killerSteam && victimSteam && killerSteam === victimSteam;
  const sameName = String(killer || '').trim().toLocaleLowerCase() === String(victim || '').trim().toLocaleLowerCase();
  if (sameSteam || (killer && victim && sameName)) return 'self';
  if (type.includes('pve') || /zombie|puppet|animal|sentry|environment/i.test(`${killer} ${event && event.raw ? event.raw : ''}`)) return 'pve';
  return 'pvp';
}

function killIconSvg() {
  return '<svg class="kf-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="9"></circle><line x1="22" y1="12" x2="18" y2="12"></line><line x1="6" y1="12" x2="2" y2="12"></line><line x1="12" y1="6" x2="12" y2="2"></line><line x1="12" y1="22" x2="12" y2="18"></line><circle cx="12" cy="12" r="2"></circle></svg>';
}

function isHeadshotKill(event) {
  const text = killEventTextBlob(event).toLowerCase();
  return /headshot|head shot|head\b|skull|brain|голов/.test(text);
}

function killBadgeCause(cause, headshot) {
  if (headshot) return 'HEAD';
  const lower = String(cause || '').toLowerCase();
  if (lower.includes('пуля') || lower.includes('projectile')) return 'SHOT';
  if (lower.includes('ближ') || lower.includes('melee')) return 'MELEE';
  if (lower.includes('взрыв') || lower.includes('explosion')) return 'EXP';
  if (lower.includes('транспорт') || lower.includes('vehicle')) return 'VEH';
  if (lower.includes('npc') || lower.includes('pve')) return 'PVE';
  return 'INFO';
}

function renderKillEvents(list) {
  if (!list.length) return '<div class="empty-state">Событий пока нет.</div>';
  const killKey = (event, index) => [
    event && (event.timestampUtc || event.utc || event.time || ''),
    pick(event, ['killerSteamId', 'killerName', 'killer', 'attackerName'], ''),
    pick(event, ['victimSteamId', 'victimName', 'victim', 'targetName'], ''),
    parseKillWeapon(event),
    index
  ].join('|');
  if (state.selectedKillKey && !list.some((event, index) => killKey(event, index) === state.selectedKillKey)) {
    state.selectedKillKey = '';
  }
  return list.map((event, index) => {
    const killer = cleanKillParticipantName(pick(event, ['killerName', 'killer', 'attackerName', 'attacker', 'sourceName'], '') || killTextParticipant(event, 'killer')) || 'Неизвестно';
    const victim = cleanKillParticipantName(pick(event, ['victimName', 'victim', 'targetName', 'target'], '') || killTextParticipant(event, 'victim')) || 'Неизвестно';
    const weaponRaw = parseKillWeapon(event);
    const weapon = killSourceLabel(event, killer, victim);
    const cause = parseKillCause(event);
    const distance = parseKillDistance(event);
    const killerLocation = parseKillLocation(event, 'killer');
    const victimLocation = parseKillLocation(event, 'victim');
    const kind = killKind(event, killer, victim);
    const headshot = isHeadshotKill(event);
    const kindLabel = kind === 'self' ? 'SELF' : (kind === 'pve' ? 'PVE' : 'PVP');
    const hasDistance = distance !== '' && Number.isFinite(Number(distance));
    const distanceLabel = hasDistance ? `${Math.round(Number(distance))} м` : '';
    const killerPos = fmtKillLocation(killerLocation);
    const victimPos = fmtKillLocation(victimLocation);
    const key = killKey(event, index);
    const open = state.selectedKillKey === key;
    const sourceTitle = [
      `Оружие: ${weapon}`,
      `Причина: ${cause || '-'}`,
      `Дистанция: ${hasDistance ? `${Number(distance).toFixed(1)} м` : '-'}`,
      `Откуда: ${killerPos}`,
      `Точка смерти: ${victimPos}`
    ].join('\n');
    return `<article class="kf-row ${escapeAttr(kind)}${headshot ? ' headshot' : ''}${open ? ' is-open' : ''}" title="${escapeAttr(sourceTitle)}">
      <button class="kf-summary" type="button" data-kill-open="${escapeAttr(key)}" aria-expanded="${open ? 'true' : 'false'}">
        <time class="kf-time">${escapeHtml(fmtChatTime(event))}</time>
        <div class="kf-combat">
          <span class="kf-killer" title="${escapeAttr(killer)}">${escapeHtml(killer)}</span>
          <div class="kf-weapon" title="${escapeAttr(weapon)}">
            <span>${escapeHtml(weapon)}</span>
            ${killIconSvg()}
          </div>
          <span class="kf-victim" title="${escapeAttr(victim)}">${escapeHtml(victim)}</span>
        </div>
        <div class="kf-badges">
          <span class="kf-badge ${escapeAttr(kind)}">${escapeHtml(kindLabel)}</span>
          ${distanceLabel ? `<span class="kf-badge dist">${escapeHtml(distanceLabel)}</span>` : ''}
          ${headshot ? '<span class="kf-badge head">HEAD</span>' : ''}
          <span class="kf-open-label">${open ? 'Скрыть' : 'Открыть'}</span>
        </div>
      </button>
      ${open ? `<div class="kf-detail">
        <span title="${escapeAttr(killerPos)}"><b>Откуда</b>${escapeHtml(killerPos)}</span>
        <span title="${escapeAttr(victimPos)}"><b>Точка смерти</b>${escapeHtml(victimPos)}</span>
      </div>` : ''}
    </article>`;
  }).join('');
}

async function refreshEconomy() {
  await ensureModulesLoaded().catch(() => {});
  const data = await api('/api/economy');
  state.economy = data || {};
  renderEconomy();
}

function renderEconomy() {
  const wallets = Array.isArray(state.economy.wallets) ? state.economy.wallets : [];
  const bank = Array.isArray(state.economy.bankAccounts) ? state.economy.bankAccounts : [];
  const recent = Array.isArray(state.economy.recent) ? state.economy.recent : (Array.isArray(state.economy) ? state.economy : []);
  const totalWallet = wallets.reduce((sum, w) => sum + Number(w.walletBalance || 0), 0);
  const totalFame = wallets.reduce((sum, w) => sum + Number(w.famePoints || 0), 0);
  const totalBank = bank.reduce((sum, b) => sum + Number(b.accountBalance || 0), 0);
  const walletHtml = wallets.slice(0, 80).map(w => `<article class="event economy-row">
    <div class="meta"><span>${escapeHtml(w.name || w.steamId || 'игрок')}</span><span>${fmtTime(w.lastLoginUtc)}</span></div>
    <div class="kv">
      <span>Кошелёк</span><b>${escapeHtml(w.walletBalance ?? 0)}</b>
      <span>Слава</span><b>${escapeHtml(w.famePoints ?? 0)}</b>
      <span>SteamID</span><b>${escapeHtml(w.steamId || '')}</b>
    </div>
  </article>`).join('');
  const bankHtml = bank.slice(0, 80).map(b => `<article class="event economy-row">
    <div class="meta"><span>${escapeHtml(b.ownerName || b.ownerSteamId || 'банковский счёт')}</span><span>${escapeHtml(b.accountNumber || '')}</span></div>
    <div class="kv">
      <span>Валюта</span><b>${escapeHtml(b.currencyType ?? '')}</b>
      <span>Баланс</span><b>${escapeHtml(b.accountBalance ?? 0)}</b>
      <span>Владелец</span><b>${escapeHtml(b.ownerSteamId || '')}</b>
    </div>
  </article>`).join('');
  const fastCfg = moduleConfig('fast-travel');
  const rentalCfg = moduleConfig('vehicle-rental');
  const scanCfg = moduleConfig('sector-scan');
  const outposts = Array.isArray(fastCfg.Outposts) ? fastCfg.Outposts : (Array.isArray(fastCfg.points) ? fastCfg.points : []);
  const vehicles = Array.isArray(rentalCfg.Vehicles) ? rentalCfg.Vehicles : (Array.isArray(rentalCfg.vehicles) ? rentalCfg.vehicles : []);
  const spendRows = []
    .concat(outposts.slice(0, 8).map(route => ({
      type: 'Телепорт',
      name: route.DisplayName || route.displayName || route.CommandAlias || route.commandAlias || 'маршрут',
      price: route.Price ?? route.price ?? fastCfg.FixedFare ?? 0,
      note: route.CommandAlias || route.commandAlias || ''
    })))
    .concat(vehicles.slice(0, 8).map(vehicle => ({
      type: 'Аренда',
      name: vehicle.DisplayName || vehicle.displayName || vehicle.Alias || vehicle.alias || vehicle.AssetName || vehicle.assetName || 'транспорт',
      price: vehicle.InitialCharge ?? vehicle.initialCharge ?? 0,
      note: `+ ${vehicle.PricePer10Minutes ?? vehicle.pricePer10Minutes ?? 0} / 10 мин`
    })))
    .concat(scanCfg.ScanCost != null || scanCfg.scanCost != null ? [{
      type: 'Скан',
      name: 'Скан сектора',
      price: scanCfg.ScanCost ?? scanCfg.scanCost ?? 0,
      note: 'команда /scan'
    }] : []);
  const spendHtml = spendRows.length ? spendRows.map(row => `<article class="economy-spend-row">
    <span>${escapeHtml(row.type)}</span>
    <b>${escapeHtml(row.name)}</b>
    <strong>${escapeHtml(row.price)}</strong>
    <em>${escapeHtml(row.note || '')}</em>
  </article>`).join('') : '<div class="event">Платные действия пока не найдены в конфигах плагинов.</div>';
  els.economyList.innerHTML = `
    <div class="economy-kpis" data-economy-section="overview">
      <article><span>Кошельков</span><b>${wallets.length}</b></article>
      <article><span>Всего в кошельках</span><b>${totalWallet}</b></article>
      <article><span>Всего в банке</span><b>${totalBank}</b></article>
      <article><span>Очков славы</span><b>${totalFame}</b></article>
    </div>
    <div class="economy-groups">
      <section class="economy-group" data-economy-section="overview">
        <div class="economy-group-head"><h3>Как пользоваться экономикой</h3><span>быстрая памятка</span></div>
        <div class="economy-help-grid">
          <article><b>Начислить игроку</b><span>Открой карточку игрока → Экономика → укажи сумму.</span></article>
          <article><b>Посмотреть расходы</b><span>Вкладка «Цены» показывает платные телепорты, аренду и скан.</span></article>
          <article><b>Проверить историю</b><span>Вкладка «Операции» показывает последние изменения денег.</span></article>
        </div>
      </section>
      <section class="economy-group" data-economy-section="prices" hidden>
        <div class="economy-group-head"><h3>Где списываются деньги</h3><span>цены из настроек плагинов</span></div>
        <div class="economy-spend-list">${spendHtml}</div>
      </section>
      <section class="economy-group" data-economy-section="wallets" hidden>
        <div class="economy-group-head"><h3>Кошельки игроков</h3><span>начисление делай из карточки игрока</span></div>
        ${walletHtml || '<div class="event">В базе пока нет кошельков игроков.</div>'}
      </section>
      <section class="economy-group" data-economy-section="bank" hidden>
        <div class="economy-group-head"><h3>Банковские счета</h3><span>балансы из игры</span></div>
        ${bankHtml || '<div class="event">В базе пока нет банковских счетов.</div>'}
      </section>
      <section class="economy-group" data-economy-section="recent" hidden>
        <div class="economy-group-head"><h3>Последние операции</h3><span>что недавно менялось</span></div>
        ${renderEvents(recent)}
      </section>
    </div>`;
  setEconomyTab(state.economyTab || 'overview');
}

async function refreshDownloads() {
  const data = await api('/api/downloads');
  state.downloads = Array.isArray(data) ? data : [];
  els.downloadsList.innerHTML = state.downloads.length ? state.downloads.map(file => `<article class="download">
    <b>${escapeHtml(file.name || file.path || 'файл')}</b>
    <span>${escapeHtml(file.description || '')}</span>
    <code>${escapeHtml(file.path || '')}</code>
  </article>`).join('') : '<div class="event">Список файлов недоступен.</div>';
}

async function refreshCatalogs() {
  const [staticItems, staticVehicles, skills] = await Promise.all([
    fetchCachedJsonAsset('/spawnable-items-lite.json').then(data => data || fetchCachedJsonAsset('/spawnable-items.json')),
    fetchCachedJsonAsset('/spawnable-vehicles.json'),
    api('/api/player/skill-catalog').catch(() => [])
  ]);
  let items = unwrapCatalogData(staticItems, 'items');
  let vehicles = unwrapCatalogData(staticVehicles, 'vehicles');
  if (!items.length) items = unwrapCatalogData(await api('/api/catalog/items').catch(() => []), 'items');
  if (!vehicles.length) vehicles = unwrapCatalogData(await api('/api/catalog/vehicles').catch(() => []), 'vehicles');
  state.items = items.map(entry => normalizeCatalogEntry(entry, 'item'));
  state.vehicles = vehicles.map(entry => normalizeCatalogEntry(entry, 'vehicle'));
  if (state.itemIconAssetsLoaded) {
    refreshCatalogIconAliases();
  } else {
    loadItemIconAssets({ rerender: true }).catch(() => {});
  }
  state.skillCatalog = mergeSkillCatalog(skills);
  if (els.itemCatalog) {
    els.itemCatalog.innerHTML = state.items.map(item => `<option value="${escapeAttr(item.itemId || item.id || '')}">${escapeHtml(item.name || item.category || '')}</option>`).join('');
  }
  if (els.vehicleCatalog) {
    els.vehicleCatalog.innerHTML = state.vehicles.map(vehicle => `<option value="${escapeAttr(vehicle.vehicleId || vehicle.id || '')}">${escapeHtml(vehicle.name || vehicle.category || '')}</option>`).join('');
  }
  if (els.skillCatalog) {
    els.skillCatalog.innerHTML = [
      '<option value="allskills">Все навыки сразу - уровень из поля рядом</option>',
      '<option value="AllSkills">Все навыки сразу - режим Wargm</option>'
    ].concat(state.skillCatalog.map(skillOptionHtml)).join('');
  }
  renderCharacterPackCatalog();
  renderModuleCatalog();
}

async function refreshModules() {
  const modules = ensurePanelModules(await api('/api/plugins'));
  state.modules = await hydratePanelModules(modules);
  rememberModuleConfigBaselines(state.modules);
  if (!state.selectedModule && state.modules.length) state.selectedModule = state.modules[0].key;
  renderModules();
  renderCharacterPackCatalog();
}

function moduleConfigFingerprint(config) {
  try {
    const value = JSON.stringify(config);
    return typeof value === 'string' ? value : '';
  } catch (_) {
    return '';
  }
}

function rememberModuleConfigBaselines(modules) {
  const baselines = {};
  (Array.isArray(modules) ? modules : []).forEach(module => {
    const key = String(module && module.key || '').trim().toLowerCase();
    const fingerprint = !module || module.synthetic ? '' : moduleConfigFingerprint(module.config);
    if (key && fingerprint) baselines[key] = fingerprint;
  });
  state.moduleConfigBaselines = baselines;
}

function defaultBattlepassItemsText(day, vip = false) {
  if (day === 2) return vip ? 'Apple_2|4' : 'Apple_2|2';
  if (day === 5) return vip ? 'CannedGoulash|2' : 'CannedGoulash|1';
  if (day === 10) return vip ? 'Emergency_bandage_Big|2' : 'Emergency_bandage_Big|1';
  if (day === 20) return vip ? 'Apple_2|4;CannedGoulash|2' : 'Apple_2|2;CannedGoulash|1';
  if (day === 30) return vip ? 'MRE_Cheeseburger|2;Emergency_bandage_Big|2' : 'MRE_Cheeseburger|1;Emergency_bandage_Big|1';
  return '';
}

function defaultBattlepassRewards(vip = false) {
  return Array.from({ length: 30 }, (_, index) => {
    const day = index + 1;
    const money = vip
      ? (day === 1 ? 5000 : day === 2 ? 8000 : day === 3 ? 3000 : 2500 + day * 400)
      : (day === 1 ? 3000 : day === 2 ? 5000 : day === 3 ? 1000 : 1000 + day * 250);
    const gold = vip ? (day % 5 === 0 ? 2 : 0) : ([7, 14, 21, 30].includes(day) ? 1 : 0);
    const fame = vip ? (day % 3 === 0 ? 50 : 0) : (day % 5 === 0 ? 25 : 0);
    return {
      Enabled: true,
      Day: day,
      MoneyAmount: money,
      GoldAmount: gold,
      FameAmount: fame,
      ItemsText: defaultBattlepassItemsText(day, vip),
      Message: vip ? 'VIP Battlepass награда {day}/{maxDays} получена.' : 'Battlepass награда {day}/{maxDays} получена.'
    };
  });
}

function ensurePanelModules(modules) {
  const list = Array.isArray(modules) ? modules.slice() : [];
  const keys = new Set(list.map(m => String(m && m.key || '').toLowerCase()));
  if (!keys.has('daily-pack')) {
    list.push({
      key: 'daily-pack',
      name: 'Ежедневный пак',
      description: 'Ежедневная выдача предметов через безопасную очередь WelcomePack.',
      category: 'players',
      enabled: true,
      config: {
        Name: 'Ежедневный пак',
        Description: 'Ежедневная выдача предметов игроку через безопасную очередь WelcomePack.',
        Enabled: true,
        CooldownHours: 24,
        RequiredPermission: '',
        SuccessMessage: 'Ежедневный пак выдан.',
        VipItems: [
          { ItemId: 'Apple_2', Quantity: 1 }
        ],
        VipSettings: {
          Enabled: true,
          RequiredPermission: 'daily-pack.vip',
          CooldownHours: 12,
          MoneyAmount: 5000,
          GoldAmount: 5,
          FameAmount: 50,
          SuccessMessage: 'VIP бонус ежедневного пака выдан.'
        },
        Items: [
          { ItemId: 'Apple_2', Quantity: 2 },
          { ItemId: 'CannedGoulash', Quantity: 1 },
          { ItemId: 'Emergency_bandage_Big', Quantity: 1 }
        ]
      },
      synthetic: true
    });
  }
  if (!keys.has('battlepass')) {
    list.push({
      key: 'battlepass',
      name: 'Battlepass',
      description: '30-дневная серия наград за реальные дни входа после прогрузки игрока.',
      category: 'players',
      enabled: true,
      config: {
        Name: 'Battlepass',
        Description: '30-дневная серия наград за реальные дни входа.',
        Enabled: true,
        MaxDays: 30,
        DelayAfterJoinSeconds: 60,
        PollIntervalMs: 5000,
        RetryWindowSeconds: 300,
        InterItemDelayMs: 1500,
        RequiredPermission: '',
        SuccessMessage: 'Battlepass награда {day}/{maxDays} выдана.',
        VipSettings: { Enabled: true, RequiredPermission: 'battlepass.vip', SuccessMessage: 'VIP Battlepass награда {day}/{maxDays} выдана.' },
        Rewards: defaultBattlepassRewards(false),
        VipRewards: defaultBattlepassRewards(true)
      },
      synthetic: true
    });
  }
  if (!keys.has('scheduled-events')) {
    list.push({
      key: 'scheduled-events',
      name: 'Планировщик заданий',
      description: 'События сервера по времени: команды, cargo drop, world events и наборы предметов в точках карты.',
      category: 'server',
      enabled: true,
      config: {
        Enabled: true,
        Name: 'Планировщик заданий',
        Description: 'Задания сервера по времени или интервалу.',
        PollIntervalMs: 15000,
        MaxRunsPerTick: 1,
        WorldEventClasses: ['BP_CargoDropEvent', 'BP_EncounterCargoDropEvent', 'BP_DeathmatchGameEvent', 'BP_TeamDeathmatchGameEvent', 'BP_CTFGameEvent', 'BP_DropZoneGameEvent'],
        Jobs: [
          { Enabled: false, Name: 'Новое задание', Mode: 'Command', ScheduleTimes: '12:00', IntervalMinutes: 60, RunOnStartup: false, PointGroup: '', PointCount: 1, ItemSet: '', WorldEventClass: '', CommandTemplate: 'Announce Плановое сообщение сервера.', Announcement: '', MaxItemsPerRun: 1 }
        ],
        Points: [],
        ItemSets: []
      },
      synthetic: true
    });
  }
  const lightweightFallbacks = [
    ['vip-system', 'VIP игроки', 'VIP-участники по SteamID64, сроки, уровни, права и отдельные лимиты модулей.', 'players', {
      Enabled: true,
      DefaultTier: 'vip',
      DefaultDurationDays: 30,
      ExtendExisting: true,
      BasePermissions: ['vip.active', 'welcome-pack.vip', 'sethome.vip', 'daily-pack.vip', 'battlepass.vip', 'sector-scan.vip', 'vehicle-rental.vip', 'base-loot.vip'],
      TierPermissions: { premium: ['vip.premium'] },
      Members: [],
      Features: {
        WelcomePack: { Enabled: true, Items: [] },
        DailyPack: { Enabled: true, CooldownHours: 12, Items: [] },
        Battlepass: { Enabled: true },
        HomeSystem: { Enabled: true, MaxHomes: 5 },
        SectorScan: { Enabled: true, Free: true, CooldownSeconds: 30 },
        VehicleRental: { Enabled: true, DiscountPercent: 25, DefaultMinutes: 30, MaxMinutes: 180, SpawnCooldownSeconds: 60 },
        BaseLootCollector: { Enabled: true, RadiusCm: 7000, CooldownSeconds: 60, MaxItemsPerRun: 150 }
      }
    }],
    ['money-transfer', 'Переводы денег', 'Игроки переводят деньги друг другу через /sendmoney.', 'commerce', { Enabled: true }],
    ['item-upgrade', 'Апгрейд предметов', 'Платная замена предмета в руках на настроенный результат.', 'commerce', { Enabled: false, AllowReplacement: false, Rules: [] }],
    ['base-loot-collector', 'Сбор лута базы', 'Команда /loot собирает лежащий лут в сундуки своего флага.', 'players', { Enabled: false, Rules: [] }],
    ['info-response', 'Ответ команды /info', 'Настраиваемое многострочное описание команд с безопасным возвратом к динамической информации.', 'chat', {
      Name: 'Ответ команды /info',
      Description: 'Если включён свой текст, каждая непустая строка отправляется игроку отдельным сообщением чата. Пустой английский текст использует русскую версию; выключенный модуль сохраняет стандартное динамическое описание команд.',
      Enabled: true,
      UseCustomText: false,
      Text: '',
      EnglishText: '',
      LineDelayMs: 160,
      MaxLines: 24,
      MaxLineBytes: 220
    }],
    ['command-aliases', 'Команды игроков', 'Настройка слов вызова /help, /info, /rent, /loot и других команд. Применяется после рестарта SCUM-сервера.', 'chat', { Enabled: true, IncludeBangAliases: true, Help: ['help'], Info: ['info'], Rent: ['rent'], BaseLoot: ['loot', 'collectloot', 'sortloot'], Dlc: ['dlc', 'dls', 'длс'], GameStores: ['pay', 'gamestores', 'gs'] }],
    ['zone-robot-schedule', 'Роботы по зонам', 'Расписание зонных команд для роботов/событий.', 'events', { Enabled: false, Rules: [] }],
    ['panel-quests', 'Квесты сервера', 'Квестовая доска панели с наградами.', 'events', { Enabled: false, Quests: [] }]
  ];
  lightweightFallbacks.forEach(([key, name, description, category, config]) => {
    if (!keys.has(key)) {
      list.push({ key, name, description, category, enabled: config.Enabled !== false, config, synthetic: true });
      keys.add(key);
    }
  });
  return list;
}

async function hydratePanelModules(list) {
  const hydrateKeys = ['daily-pack', 'battlepass', 'vip-system', 'scheduled-events', 'money-transfer', 'item-upgrade', 'base-loot-collector', 'info-response', 'command-aliases', 'zone-robot-schedule', 'panel-quests'];
  const fallbackMeta = {
    'daily-pack': ['Ежедневный пак', 'Ежедневная выдача предметов через безопасную очередь WelcomePack.', 'players'],
    'battlepass': ['Battlepass', '30-дневная серия наград за вход.', 'players'],
    'vip-system': ['VIP игроки', 'VIP-участники, сроки и права модулей.', 'players'],
    'scheduled-events': ['Планировщик заданий', 'События сервера по времени.', 'server'],
    'money-transfer': ['Переводы денег', 'Игроки переводят деньги друг другу.', 'commerce'],
    'item-upgrade': ['Апгрейд предметов', 'Платная замена предмета в руках.', 'commerce'],
    'base-loot-collector': ['Сбор лута базы', 'Команда /loot собирает лежащий лут в сундуки своего флага.', 'players'],
    'info-response': ['Ответ команды /info', 'Настраиваемое многострочное описание команд с безопасным возвратом к динамической информации.', 'chat'],
    'command-aliases': ['Команды игроков', 'Настройка слов вызова команд. Применяется после рестарта SCUM-сервера.', 'chat'],
    'zone-robot-schedule': ['Роботы по зонам', 'Расписание зонных команд.', 'events'],
    'panel-quests': ['Квесты сервера', 'Панельная квестовая доска.', 'events']
  };
  await Promise.all(hydrateKeys.map(async key => {
    try {
      const data = await api(`/api/plugin-config?name=${encodeURIComponent(key)}`);
      const config = data && data.config ? data.config : data;
      if (!config) return;
      const meta = fallbackMeta[key] || [key, '', 'server'];
      const existing = list.find(module => String(module && module.key || '').toLowerCase() === key);
      const module = existing || {
        key,
        name: config.Name || meta[0],
        description: config.Description || meta[1],
        category: meta[2]
      };
      module.config = config;
      module.name = config.Name || module.name || key;
      module.description = config.Description || module.description || '';
      module.enabled = moduleIsEnabled(module);
      // The native endpoint reports legacy-alias state separately from the config.
      // Keep this read-only: the panel must never rename, merge, or delete a file.
      module.configDiagnostics = {
        legacyAliasConflict: data && data.legacyAliasConflict === true,
        migrationRequired: data && data.migrationRequired === true,
        runtimeConfigActive: !data || data.runtimeConfigActive !== false
      };
      module.synthetic = false;
      if (!existing) list.push(module);
    } catch (_) {
      // If the server does not expose this config endpoint, keep the local fallback.
    }
  }));
  return list;
}

function moduleConfig(key) {
  const mod = state.modules.find(item => item.key === key);
  return mod && mod.config ? mod.config : {};
}

function characterPackOptions() {
  const cfg = moduleConfig('character-packs');
  const packs = Array.isArray(cfg.Packs) ? cfg.Packs : [];
  const presets = Array.isArray(cfg.SkillPresets) ? cfg.SkillPresets : [];
  const items = packs.map(pack => ({
    value: pack.Name || '',
    label: pack.Description || pack.Name || 'пакет'
  })).concat(presets.map(preset => ({
    value: preset.Name || preset.Skill || '',
    label: preset.Description || preset.Skill || 'пресет навыка'
  }))).filter(entry => entry.value);
  if (!items.some(entry => String(entry.value).toLowerCase() === 'fullstats')) {
    items.unshift({ value: 'fullstats', label: 'Полный пресет атрибутов' });
  }
  return items;
}

function renderCharacterPackCatalog() {
  if (!els.characterPackCatalog) return;
  els.characterPackCatalog.innerHTML = characterPackOptions()
    .map(entry => `<option value="${escapeAttr(entry.value)}">${escapeHtml(entry.label || '')}</option>`)
    .join('');
}

async function ensureModulesLoaded() {
  if (!state.modules.length) {
    state.modules = ensurePanelModules(await api('/api/plugins'));
    renderCharacterPackCatalog();
    updatePlayerBattlepassControls();
  }
}

async function refreshHomes() {
  const data = await api('/api/home/list');
  state.homes = Array.isArray(data) ? data : [];
  renderHomes();
}

function renderHomes() {
  if (!els.homeList) return;
  els.homeList.innerHTML = state.homes.length ? state.homes.slice().reverse().slice(0, 12).map(home => `<article class="event home-row">
    <div class="meta"><span>${escapeHtml(home.label || 'home')}</span><span>${fmtTime(home.utc)}</span></div>
    <div>${escapeHtml(home.name || home.steamId || '')}: ${Number(home.x || 0).toFixed(0)}, ${Number(home.y || 0).toFixed(0)}, ${Number(home.z || 0).toFixed(0)}</div>
    <button class="btn" data-home-label="${escapeAttr(home.label || 'home')}" data-home-x="${escapeAttr(home.x || 0)}" data-home-y="${escapeAttr(home.y || 0)}" data-home-z="${escapeAttr(home.z || 0)}">Телепорт</button>
  </article>`).join('') : '<div class="event">Сохранённых точек пока нет.</div>';
}

async function refreshWelcomeTimers() {
  state.welcomeTimersLoading = true;
  updatePlayerWelcomeControls();
  const [timers, claims] = await Promise.all([
    api('/api/welcome-pack/timers').catch(() => []),
    api('/api/module-state', { method: 'POST', body: JSON.stringify({ key: 'welcome-pack' }) }).catch(() => [])
  ]);
  state.welcomeTimers = mergeWelcomeTimersWithOptimistic(Array.isArray(timers) ? timers : []);
  state.welcomeClaims = Array.isArray(claims) ? claims : [];
  state.welcomeTimersLoaded = true;
  state.welcomeTimersLoading = false;
  renderWelcomeTimers();
  updatePlayerWelcomeControls();
}

function renderWelcomeTimers() {
  if (!els.welcomeTimers) return;
  const latestByPlayer = new Map();
  const keyFor = row => String(row.steamId || row.steam || row.name || row.userProfileId || 'unknown').toLowerCase();
  for (const timer of state.welcomeTimers || []) {
    const key = keyFor(timer);
    latestByPlayer.set(key, Object.assign({}, latestByPlayer.get(key) || {}, { timer }));
  }
  for (const claim of state.welcomeClaims || []) {
    const key = keyFor(claim);
    const current = latestByPlayer.get(key) || {};
    current.claims = current.claims || [];
    current.claims.push(claim);
    latestByPlayer.set(key, current);
  }
  const rows = Array.from(latestByPlayer.values()).map(entry => {
    const claims = (entry.claims || []).slice().sort((a, b) => String(b.utc || '').localeCompare(String(a.utc || '')));
    const claim = claims[0] || {};
    const timer = entry.timer || {};
    const result = claim.result && typeof claim.result === 'object' ? claim.result : null;
    const ok = claim.ok === true || (result && result.ok === true);
    const failed = claim.ok === false || (result && result.ok === false);
    return {
      steamId: claim.steamId || timer.steamId || '',
      name: claim.name || timer.name || '',
      ok,
      failed,
      claimUtc: claim.utc || timer.utc || '',
      expiresAtUtc: timer.expiresAtUtc || '',
      cooldownHours: timer.cooldownHours ?? '',
      message: claim.message || (result && result.error) || ''
    };
  }).sort((a, b) => String(b.claimUtc || '').localeCompare(String(a.claimUtc || '')));
  els.welcomeTimers.innerHTML = rows.length ? `<div class="welcome-claims">
    <div class="welcome-claims-head">
      <b>Игрок</b><b>SteamID</b><b>Получал</b><b>Следующий доступ</b><b></b>
    </div>
    ${rows.map(row => {
      const status = row.ok ? 'Получил' : (row.failed ? 'Ошибка' : 'Ожидает');
      const statusClass = row.ok ? 'good-text' : (row.failed ? 'bad-text' : 'muted-line');
      const resetTarget = row.steamId || row.name || '';
      return `<article class="welcome-claim-row">
        <span><b>${escapeHtml(row.name || 'игрок')}</b><small>${escapeHtml(row.message || '')}</small></span>
        <span>${escapeHtml(row.steamId || '-')}</span>
        <span class="${statusClass}">${escapeHtml(status)}${row.claimUtc ? `<small>${escapeHtml(fmtTime(row.claimUtc))}</small>` : ''}</span>
        <span>${row.expiresAtUtc ? escapeHtml(fmtTime(row.expiresAtUtc)) : (row.cooldownHours !== '' ? `${escapeHtml(row.cooldownHours)} ч.` : '-')}</span>
        <button class="mini-action danger" type="button"
          data-welcome-reset="${escapeAttr(resetTarget)}"
          data-welcome-steam="${escapeAttr(row.steamId || '')}"
          data-welcome-name="${escapeAttr(row.name || '')}">Сбросить</button>
      </article>`;
    }).join('')}
  </div>` : '<div class="event">Пока нет записей, кто получал стартовый набор.</div>';
}

function welcomeIdentityFrom(row) {
  return {
    steam: String(row && (row.steamId || row.steam || row.targetSteamId) || '').trim(),
    name: String(row && (row.name || row.targetName || row.playerName) || '').trim().toLowerCase(),
    profile: String(row && (row.userProfileId || row.serverUserProfileId || row.profileId || row.ProfileId) || '').trim()
  };
}

function welcomeTargetIdentity(target = state.selectedPlayerTarget) {
  const live = target ? findPlayerByIdentity(target.steamId, target.name, target.runtimeKey) : null;
  return {
    steam: String(target && target.steamId || live && (live.steamId || live.SteamId || live.steam) || '').trim(),
    name: String(target && target.name || live && (live.name || live.Name || live.playerName) || '').trim().toLowerCase(),
    profile: String(target && target.profileId || live && (live.userProfileId || live.serverUserProfileId || live.profileId || live.ProfileId) || '').trim()
  };
}

function welcomeRowMatchesTarget(row, targetIdentity) {
  const rowId = welcomeIdentityFrom(row);
  if (targetIdentity.steam && rowId.steam && targetIdentity.steam === rowId.steam) return true;
  if (targetIdentity.profile && rowId.profile && targetIdentity.profile === rowId.profile) return true;
  return Boolean(targetIdentity.name && rowId.name && targetIdentity.name === rowId.name);
}

function mergeWelcomeTimersWithOptimistic(timers) {
  const rows = Array.isArray(timers) ? timers.slice() : [];
  for (const optimistic of state.welcomeOptimisticTimers || []) {
    const identity = welcomeIdentityFrom(optimistic);
    const exists = rows.some(row => welcomeRowMatchesTarget(row, identity));
    if (!exists) rows.push(optimistic);
  }
  return rows;
}

function markWelcomePackReceivedLocal(target = modalTargetBody()) {
  const now = new Date().toISOString();
  const row = {
    utc: now,
    at: Math.floor(Date.now() / 1000),
    steamId: target.steamId || target.steam || '',
    name: target.name || '',
    userProfileId: target.profileId || target.userProfileId || '',
    optimistic: true
  };
  const identity = welcomeIdentityFrom(row);
  state.welcomeOptimisticTimers = (state.welcomeOptimisticTimers || []).filter(existing => !welcomeRowMatchesTarget(existing, identity));
  state.welcomeOptimisticTimers.push(row);
  state.welcomeTimers = mergeWelcomeTimersWithOptimistic(state.welcomeTimers || []);
  state.welcomeTimersLoaded = true;
  state.welcomeTimersLoading = false;
  renderWelcomeTimers();
  updatePlayerWelcomeControls();
}

function clearWelcomePackReceivedLocal(target = modalTargetBody()) {
  const identity = welcomeTargetIdentity(target);
  state.welcomeOptimisticTimers = (state.welcomeOptimisticTimers || []).filter(row => !welcomeRowMatchesTarget(row, identity));
  state.welcomeTimers = (state.welcomeTimers || []).filter(row => !welcomeRowMatchesTarget(row, identity));
  renderWelcomeTimers();
  updatePlayerWelcomeControls();
}

function welcomePackStatusFor(target = state.selectedPlayerTarget) {
  const targetIdentity = welcomeTargetIdentity(target);
  if (!targetIdentity.steam && !targetIdentity.name && !targetIdentity.profile) return { received: false, timer: null };
  const timers = (state.welcomeTimers || [])
    .filter(row => welcomeRowMatchesTarget(row, targetIdentity))
    .sort((a, b) => String(b.utc || '').localeCompare(String(a.utc || '')));
  return { received: timers.length > 0, timer: timers[0] || null };
}

function updatePlayerWelcomeControls() {
  const claimButtons = [els.playerActionWelcome, els.playerActionWelcomeProfile, els.playerActionWelcomeCharacter].filter(Boolean);
  const resetButtons = [els.playerActionWelcomeReset, els.playerActionWelcomeResetProfile, els.playerActionWelcomeResetCharacter].filter(Boolean);
  if (!claimButtons.length) return;
  const status = welcomePackStatusFor();
  const hasTarget = Boolean(state.selectedPlayerTarget && (state.selectedPlayerTarget.steamId || state.selectedPlayerTarget.name || state.selectedPlayerTarget.profileId));
  const checking = hasTarget && (!state.welcomeTimersLoaded || state.welcomeTimersLoading);
  const strips = [els.playerWelcomeStrip, els.playerWelcomeStripProfile].filter(Boolean);
  strips.forEach(strip => {
    strip.classList.toggle('is-checking', checking);
    strip.classList.toggle('is-received', status.received);
  });
  const statusText = checking ? 'Проверяю стартпак' : (status.received ? 'Стартпак получен' : 'Стартпак не получен');
  [els.playerWelcomeStatus, els.playerWelcomeStatusProfile].filter(Boolean).forEach(label => { label.textContent = statusText; });
  const timerText = status.timer && (status.timer.utc || status.timer.timestampUtc || status.timer.time)
    ? `Последняя отметка: ${formatShortDateTime(status.timer.utc || status.timer.timestampUtc || status.timer.time)}.`
    : 'Выдача и снятие отметки находятся в окне выдачи предметов.';
  const hintText = checking ? 'Читаю отметки стартового набора...' : timerText;
  [els.playerWelcomeHint, els.playerWelcomeHintProfile].filter(Boolean).forEach(label => { label.textContent = hintText; });
  claimButtons.forEach(btn => {
    btn.disabled = !hasTarget || checking || status.received;
    btn.textContent = checking ? 'Проверяю стартпак' : (status.received ? 'Стартпак получен' : 'Выдать стартпак');
    btn.classList.toggle('primary', hasTarget && !checking && !status.received);
    btn.classList.toggle('welcome-received', status.received);
  });
  resetButtons.forEach(btn => {
    btn.hidden = !hasTarget;
    btn.disabled = !hasTarget || checking || !status.received;
    btn.textContent = checking ? 'Проверка...' : (status.received ? 'Снять стартпак' : 'Стартпак не получен');
  });
}

function updatePlayerBattlepassControls() {
  const strips = [els.playerBattlepassStrip, els.playerBattlepassStripProfile].filter(Boolean);
  if (!strips.length) return;
  const cfg = moduleConfig('battlepass');
  const enabled = (cfg.Enabled ?? cfg.enabled ?? false) !== false && Boolean(cfg.Enabled ?? cfg.enabled ?? false);
  const maxDays = Number(cfg.MaxDays ?? cfg.maxDays ?? 30) || 30;
  const delay = Math.max(60, Number(cfg.DelayAfterJoinSeconds ?? cfg.delayAfterJoinSeconds ?? cfg.JoinDelaySeconds ?? cfg.joinDelaySeconds ?? 60) || 60);
  const vipSettings = cfg.VipSettings || cfg.vipSettings || {};
  const vipEnabled = (vipSettings.Enabled ?? vipSettings.enabled ?? false) !== false && Boolean(vipSettings.Enabled ?? vipSettings.enabled ?? false);
  const statusText = enabled ? 'Battlepass включён' : 'Battlepass отключён';
  const vipText = vipEnabled ? 'VIP-награды включены отдельно.' : 'VIP-награды отключены.';
  const hintText = enabled
    ? `Автоматическая выдача до ${maxDays} дней через ${delay} сек после входа. Команда игрока: /battlepass или /bp. ${vipText}`
    : 'Модуль выключен. Включается в Плагины -> Battlepass.';
  strips.forEach(strip => {
    strip.classList.toggle('is-received', enabled);
    strip.classList.toggle('is-checking', !enabled);
  });
  [els.playerBattlepassStatus, els.playerBattlepassStatusProfile].filter(Boolean).forEach(label => { label.textContent = statusText; });
  [els.playerBattlepassHint, els.playerBattlepassHintProfile].filter(Boolean).forEach(label => { label.textContent = hintText; });
}

async function refreshServices() {
  await ensureModulesLoaded();
  if (!state.players.length) await refreshPlayers().catch(() => {});
  const fast = moduleConfig('fast-travel');
  const routes = Array.isArray(fast.Outposts) ? fast.Outposts : (Array.isArray(fast.outposts) ? fast.outposts : (Array.isArray(fast.points) ? fast.points : []));
  els.fastRoute.innerHTML = '<option value="">Координаты вручную</option>' + routes.map(route => {
    const pointValue = route.ArrivalPoint || route.arrivalPoint || route.point || [];
    const point = Array.isArray(pointValue) ? pointValue.join(',') : '';
    const alias = route.CommandAlias || route.commandAlias || route.alias || '';
    const price = route.Price ?? route.price ?? '';
    return `<option value="${escapeAttr(alias)}" data-point="${escapeAttr(point)}" data-price="${escapeAttr(price)}">${escapeHtml(route.DisplayName || route.displayName || alias || 'маршрут')}</option>`;
  }).join('');
  if (els.fastFare && !els.fastFare.value) {
    const fixedFare = fast.FixedFare ?? fast.fixedFare ?? fast.TravelCost ?? fast.travelCost ?? fast.Amount ?? fast.amount ?? '';
    els.fastFare.value = fixedFare;
  }
  updateFastTravelPricePreview();

  const rental = moduleConfig('vehicle-rental');
  const vehicles = Array.isArray(rental.Vehicles) ? rental.Vehicles : (Array.isArray(rental.vehicles) ? rental.vehicles : []);
  els.rentalVehicle.innerHTML = vehicles.map(vehicle => {
    const asset = vehicle.AssetName || vehicle.assetName || vehicle.vehicleId || '';
    const alias = vehicle.Alias || vehicle.alias || '';
    const defaultMinutes = vehicle.DefaultMinutes ?? vehicle.defaultMinutes ?? rental.DefaultRentalMinutes ?? rental.defaultRentalMinutes ?? 10;
    const minMinutes = vehicle.MinMinutes ?? vehicle.minMinutes ?? rental.MinRentalMinutes ?? rental.minRentalMinutes ?? 1;
    const maxMinutes = vehicle.MaxMinutes ?? vehicle.maxMinutes ?? rental.MaxRentalMinutes ?? rental.maxRentalMinutes ?? 60;
    const penalty = vehicle.MissingVehiclePenalty ?? vehicle.missingVehiclePenalty ?? rental.DefaultMissingVehiclePenalty ?? rental.defaultMissingVehiclePenalty ?? 0;
    return `<option value="${escapeAttr(asset)}" data-alias="${escapeAttr(alias)}" data-default-minutes="${escapeAttr(defaultMinutes)}" data-min-minutes="${escapeAttr(minMinutes)}" data-max-minutes="${escapeAttr(maxMinutes)}" data-penalty="${escapeAttr(penalty)}">${escapeHtml(vehicle.DisplayName || vehicle.displayName || asset || alias)}</option>`;
  }).join('');
  applyRentalVehicleDefaults();
  await refreshWelcomeTimers().catch(toast);
  await refreshHomes().catch(toast);
}

function applyRentalVehicleDefaults() {
  if (!els.rentalVehicle || !els.rentalMinutes) return;
  const opt = els.rentalVehicle.selectedOptions[0];
  if (!opt) return;
  const min = Number(opt.dataset.minMinutes || 1);
  const max = Number(opt.dataset.maxMinutes || 1440);
  const value = Number(opt.dataset.defaultMinutes || els.rentalMinutes.value || min);
  els.rentalMinutes.min = String(min || 1);
  els.rentalMinutes.max = String(max || 1440);
  if (!els.rentalMinutes.value || Number(els.rentalMinutes.value) < min || Number(els.rentalMinutes.value) > max) {
    els.rentalMinutes.value = String(Math.min(Math.max(value || min, min), max));
  }
}

function moduleIsEnabled(module) {
  const config = module && module.config ? module.config : {};
  if (Object.prototype.hasOwnProperty.call(config, 'Enabled')) return config.Enabled !== false;
  if (Object.prototype.hasOwnProperty.call(config, 'enabled')) return config.enabled !== false;
  return module && module.enabled !== false;
}

function moduleCardCode(module) {
  const key = String(module && module.key || '').toLowerCase();
  const codes = {
    'welcome-pack': 'WP',
    'daily-pack': 'DP',
    'battlepass': 'BP',
    'vip-system': 'VIP',
    'wargm-shop': 'WG',
    'gamestores-shop': 'GS',
    'vehicle-rental': 'VR',
    'money-transfer': 'MT',
    'item-upgrade': 'UP',
    'fast-travel': 'FT',
    'home-system': 'HM',
    'base-loot-collector': 'LC',
    'command-aliases': 'CMD',
    'character-packs': 'SK',
    'scheduled-events': 'SE',
    'zone-robot-schedule': 'RS',
    'panel-quests': 'Q',
    'bounty-hunt': 'BH',
    'server-kill-feed': 'KF',
    'sector-scan': 'SC',
    'private-messages': 'PM',
    'discord-log': 'DS'
  };
  return codes[key] || shortCode(module && (module.name || module.key), 'PL').slice(0, 3);
}

function moduleCardIcon(module) {
  const key = String(module && module.key || '').toLowerCase();
  const code = moduleCardCode(module);
  const icon = {
    'welcome-pack': '<path d="M6 11h12v9H6z"/><path d="M12 11v9"/><path d="M5 8h14v3H5z"/><path d="M9 8c-2.7-.5-3.2-3.4-.5-3.4 1.7 0 2.4 1.7 3.5 3.4 1.1-1.7 1.8-3.4 3.5-3.4 2.7 0 2.2 2.9-.5 3.4"/>',
    'daily-pack': '<path d="M7 3h10v3H7z"/><path d="M6 6h12v15H6z"/><path d="M9 10h6"/><path d="M9 14h6"/><path d="M9 18h3"/>',
    'battlepass': '<path d="M7 4h10l2 5-7 11L5 9z"/><path d="M5 9h14"/><path d="M9 4l3 16 3-16"/>',
    'vip-system': '<path d="M5 8l3 3 4-7 4 7 3-3v10H5z"/><path d="M5 18h14"/>',
    'wargm-shop': '<path d="M6 8h13l-1.4 8H8z"/><path d="M6 8 5 5H3"/><circle cx="9" cy="20" r="1.3"/><circle cx="16" cy="20" r="1.3"/>',
    'gamestores-shop': '<path d="M6 8h13l-1.4 8H8z"/><path d="M6 8 5 5H3"/><path d="M9 12h7"/><path d="M9 15h5"/><circle cx="9" cy="20" r="1.3"/><circle cx="16" cy="20" r="1.3"/>',
    'vehicle-rental': '<path d="M5 15l2-6h10l2 6"/><path d="M7 15h10"/><circle cx="8" cy="18" r="2"/><circle cx="16" cy="18" r="2"/>',
    'money-transfer': '<path d="M5 8h14v10H5z"/><path d="M8 12h8"/><path d="m13 9 3 3-3 3"/><path d="M8 16h3"/>',
    'item-upgrade': '<path d="M7 17 17 7"/><path d="m14 7 3-3 3 3-3 3"/><path d="M5 19l4-1 9-9"/><path d="M5 19l1-4"/>',
    'fast-travel': '<path d="M4 12h11"/><path d="M11 7l5 5-5 5"/><path d="M17 5h3v14h-3"/>',
    'home-system': '<path d="M4 11 12 4l8 7"/><path d="M6 10v10h12V10"/><path d="M10 20v-6h4v6"/>',
    'base-loot-collector': '<path d="M5 8h14v11H5z"/><path d="M8 8V5h8v3"/><path d="M8 12h8"/><path d="m10 15 2 2 4-5"/>',
    'command-aliases': '<path d="M4 6h16v12H4z"/><path d="M8 10h8"/><path d="M8 14h5"/><path d="m16 14 2 2 2-4"/>',
    'character-packs': '<path d="M12 4v5"/><path d="M8 8h8"/><circle cx="12" cy="12" r="3"/><path d="M6 21c1.2-3 3.2-4.5 6-4.5s4.8 1.5 6 4.5"/>',
    'scheduled-events': '<rect x="5" y="5" width="14" height="15" rx="2"/><path d="M8 3v4M16 3v4M5 10h14"/><path d="M9 14h3v3H9z"/>',
    'zone-robot-schedule': '<rect x="7" y="8" width="10" height="8" rx="2"/><path d="M9 8V5h6v3"/><path d="M9 18h6"/><circle cx="10" cy="12" r="1"/><circle cx="14" cy="12" r="1"/><path d="M5 12H3M21 12h-2"/>',
    'panel-quests': '<path d="M7 4h10v16H7z"/><path d="M9 8h6M9 12h6M9 16h3"/><path d="m15 16 2 2 3-4"/>',
    'bounty-hunt': '<circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="3"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/>',
    'server-kill-feed': '<path d="M7 19 18 8"/><path d="m14 8 4-4 2 2-4 4"/><path d="M6 6l12 12"/>',
    'sector-scan': '<path d="M4 4h16v16H4z"/><path d="M8 4v16M16 4v16M4 8h16M4 16h16"/><circle cx="12" cy="12" r="2"/>',
    'private-messages': '<path d="M5 6h14v10H8l-3 3z"/><path d="M8 10h8M8 13h5"/>',
    'discord-log': '<path d="M7 8c3-2 7-2 10 0l1 8c-3 2-9 2-12 0z"/><circle cx="10" cy="13" r="1"/><circle cx="14" cy="13" r="1"/>'
  }[key] || '<path d="M5 5h14v14H5z"/><path d="M8 9h8M8 13h8M8 17h5"/>';
  return `<div class="module-icon module-icon-${escapeAttr(key || 'generic')}" title="${escapeAttr(module && (module.name || module.key) || code)}">
    <svg viewBox="0 0 24 24" aria-hidden="true">${icon}</svg>
    <span>${escapeHtml(code)}</span>
  </div>`;
}

function moduleStatusLabel(module) {
  return moduleIsEnabled(module) ? 'Активен' : 'Отключен';
}

function moduleConfigWarning(module) {
  const diagnostics = module && module.configDiagnostics;
  if (!diagnostics) return null;
  if (diagnostics.migrationRequired || diagnostics.runtimeConfigActive === false) {
    return {
      label: 'Нужна миграция',
      detail: 'Найден только legacy-конфиг. Запись из панели защищена до ручной миграции с отдельной резервной копией.'
    };
  }
  if (diagnostics.legacyAliasConflict) {
    return {
      label: 'Конфликт конфигов',
      detail: 'Найдены canonical и legacy конфиги. Runtime использует canonical-конфиг; панель ничего автоматически не удаляет и не переносит.'
    };
  }
  return null;
}

function renderModuleConfigWarning(module, compact = false) {
  const warning = moduleConfigWarning(module);
  if (!warning) return '';
  const className = compact ? 'status-badge warning' : 'module-config-warning';
  return `<span class="${className}" title="${escapeAttr(warning.detail)}">${escapeHtml(warning.label)}</span>`;
}

function renderModules() {
  document.body.dataset.selectedModule = state.selectedModule || '';
  els.moduleList.innerHTML = state.modules.map(m => `<article class="module-card module-item-card ${m.key === state.selectedModule ? 'active' : ''}" data-module="${escapeAttr(m.key)}">
    ${moduleCardIcon(m)}
    <div class="module-meta">
      <h4>${escapeHtml(m.name || m.key)}</h4>
      <span>${escapeHtml(moduleStatusLabel(m))}</span>
      ${renderModuleConfigWarning(m)}
      ${m.description ? `<p title="${escapeAttr(m.description)}">${escapeHtml(m.description)}</p>` : ''}
    </div>
  </article>`).join('');
  const selected = state.modules.find(m => m.key === state.selectedModule);
  if (els.moduleSummary) {
    const enabled = state.modules.filter(moduleIsEnabled).length;
    els.moduleSummary.innerHTML = `
      <article class="summary-card"><b>${state.modules.length}</b><span>модулей</span></article>
      <article class="summary-card"><b>${enabled}</b><span>включено</span></article>
      <article class="summary-card"><b>${escapeHtml(state.selectedModule || '-')}</b><span>выбран</span></article>`;
  }
  if (selected) {
    els.moduleTitle.innerHTML = `${escapeHtml(selected.name || selected.key)} <span class="status-badge ${moduleIsEnabled(selected) ? 'enabled' : 'disabled'}">${escapeHtml(moduleStatusLabel(selected))}</span>${renderModuleConfigWarning(selected, true)}`;
    const selectedKey = String(selected.key || '').trim().toLowerCase();
    const configVerified = !selected.synthetic && Boolean(state.moduleConfigBaselines[selectedKey]);
    if (els.saveModule) {
      els.saveModule.textContent = `Сохранить ${selected.name}`;
      els.saveModule.disabled = !configVerified;
      els.saveModule.title = configVerified ? '' : 'Сохранение заблокировано: конфиг модуля не был успешно загружен с сервера.';
    }
    state.moduleDraft = JSON.parse(JSON.stringify(selected.config || {}));
    setModuleDraftText();
    renderModuleActions(selected);
    renderModuleFields(state.moduleDraft);
  }
}

function moduleArrayActionFor(key) {
  const normalized = String(key || '').toLowerCase();
  const simpleProfile = simpleModuleProfile(normalized);
  if (simpleProfile) {
    const active = simpleModuleTab(simpleProfile);
    const slug = normalized.replace(/[^a-z0-9]+/g, '-');
    const saveId = simpleProfile.saveId || `${slug || 'module'}SaveBtn`;
    if (active === 'items') {
      return {
        arrayKey: simpleModuleArrayKey(state.moduleDraft || {}, simpleProfile),
        label: simpleProfile.addLabel || 'Добавить',
        addId: `${slug || 'module'}AddItemBtn`,
        saveId
      };
    }
    if (active === 'vip-items' && simpleModuleHasVipItems(simpleProfile)) {
      return {
        arrayKey: simpleModuleVipArrayKey(state.moduleDraft || {}, simpleProfile),
        label: simpleProfile.vipAddLabel || 'Добавить VIP товар',
        addId: `${slug || 'module'}AddVipItemBtn`,
        saveId
      };
    }
    return { saveId };
  }
  const actions = {
    'welcome-pack': { arrayKey: 'Items', label: 'Добавить предмет', addId: 'welcomePackAddItemBtn', saveId: 'welcomePackSaveBtn' },
    'daily-pack': { arrayKey: 'Items', label: 'Добавить предмет', addId: 'dailyPackAddItemBtn', saveId: 'dailyPackSaveBtn' },
    'vip-system': { arrayKey: 'Members', label: 'Добавить VIP', addId: 'vipAddMemberBtn', saveId: 'vipSaveBtn' },
    'vehicle-rental': { arrayKey: 'Vehicles', label: 'Добавить транспорт', addId: 'vehicleRentalAddVehicleBtn', saveId: 'vehicleRentalSaveBtn' },
    'fast-travel': { arrayKey: 'Outposts', label: 'Добавить маршрут', addId: 'fastTravelAddOutpostBtn', saveId: 'fastTravelSaveBtn' },
    'wargm-shop': { saveId: 'wargmSaveBtn' },
    'gamestores-shop': { saveId: 'gamestoresSaveBtn' },
    'character-packs': { arrayKey: 'SkillPresets', label: 'Добавить пресет навыков', addId: 'characterToolsAddSkillPresetBtn', saveId: 'characterToolsSaveBtn' },
    'scheduled-events': { arrayKey: 'Jobs', label: 'Добавить задание', addId: 'scheduledEventsAddJobBtn', saveId: 'scheduledEventsSaveBtn' },
    'home-system': { saveId: 'homeSystemSaveBtn' },
    'bounty-hunt': { saveId: 'bountyHuntSaveBtn' },
    'server-kill-feed': { saveId: 'serverKillFeedSaveBtn' },
    'sector-scan': { saveId: 'sectorScanSaveBtn' },
    'private-messages': { saveId: 'privateMessagesSaveBtn' },
    'discord-log': { saveId: 'discordSaveBtn' }
  };
  return actions[normalized] || { saveId: 'moduleQuickSaveBtn' };
}

function renderModuleActions(module) {
  if (!els.moduleActions) return;
  const action = moduleArrayActionFor(module && module.key);
  const stateKey = module ? module.key : '';
  const configKey = String(stateKey || '').trim().toLowerCase();
  const configVerified = Boolean(module) && !module.synthetic && Boolean(state.moduleConfigBaselines[configKey]);
  const saveDisabled = configVerified
    ? ''
    : ' disabled title="Сохранение заблокировано: конфиг модуля не был успешно загружен с сервера."';
  const shopKey = String(stateKey || '').toLowerCase() === 'gamestores-shop'
    ? 'gamestores'
    : (String(stateKey || '').toLowerCase() === 'wargm-shop' ? 'wargm' : '');
  els.moduleActions.innerHTML = `
    <button class="btn primary" id="${escapeAttr(action.saveId || 'moduleQuickSaveBtn')}" type="button" data-module-save${saveDisabled}>Сохранить изменения</button>
    ${action.arrayKey ? `<button class="btn" id="${escapeAttr(action.addId || 'moduleQuickAddBtn')}" type="button" data-module-add-array="${escapeAttr(action.arrayKey)}">${escapeHtml(action.label)}</button>` : ''}
    ${shopKey ? `<button class="btn" type="button" data-shop-queue="${escapeAttr(shopKey)}">Очередь</button>
      <button class="btn" type="button" data-shop-action="${escapeAttr(shopKey)}:sync">Синхронизировать</button>
      <button class="btn" type="button" data-shop-action="${escapeAttr(shopKey)}:deliver-pending">Выдать очередь</button>
      <button class="btn" type="button" data-shop-action="${escapeAttr(shopKey)}:confirm-delivered">Подтвердить выданное</button>` : ''}`;
}

function moduleUsesCatalog(moduleKey, draft) {
  return false;
}

function updateModuleCatalogVisibility() {
  if (!els.moduleCatalog) return;
  const visible = moduleUsesCatalog(state.selectedModule, state.moduleDraft);
  els.moduleCatalog.hidden = !visible;
  if (visible) renderModuleCatalog();
}

function renderItemBatch() {
  if (!els.itemBatchList) return;
  els.itemBatchList.innerHTML = state.itemBatch.length
    ? state.itemBatch.map((item, index) => `<span>${escapeHtml(item.itemId)} x${escapeHtml(item.quantity)} <button type="button" class="mini-action danger" data-batch-remove="${index}" title="Удалить">Удалить</button></span>`).join('')
    : '<span>Список пуст</span>';
}

const arrayEditors = {
  Jobs: [
    ['Enabled', 'Вкл', 'checkbox'],
    ['Name', 'Название', 'text'],
    ['Mode', 'Режим', 'select:Airdrop|WorldEvent|SpawnItems|Command'],
    ['ScheduleTimes', 'Время запуска (HH:mm)', 'text'],
    ['IntervalMinutes', 'Интервал, мин', 'number'],
    ['RunOnStartup', 'Запуск при старте', 'checkbox'],
    ['PointGroup', 'Группа точек', 'text'],
    ['ItemSet', 'Набор предметов', 'text'],
    ['WorldEventClass', 'Класс world event', 'select:BP_CargoDropEvent|BP_EncounterCargoDropEvent|BP_DeathmatchGameEvent|BP_TeamDeathmatchGameEvent|BP_CTFGameEvent|BP_DropZoneGameEvent'],
    ['CommandTemplate', 'Шаблон команды', 'textarea'],
    ['Announcement', 'Объявление', 'text'],
    ['MaxItemsPerRun', 'Макс. предметов', 'number']
  ],
  Points: [
    ['Name', 'Название точки', 'text'],
    ['Group', 'Группа', 'text'],
    ['X', 'X', 'number'],
    ['Y', 'Y', 'number'],
    ['Z', 'Z', 'number'],
    ['Radius', 'Радиус', 'number']
  ],
  ItemSets: [
    ['Name', 'Название набора', 'text'],
    ['Weight', 'Вес выбора', 'number'],
    ['ItemsText', 'Предметы: ItemId|Кол-во;...', 'textarea']
  ],
  BaseLootRules: [
    ['Enabled', 'Вкл', 'checkbox'],
    ['Name', 'Название правила', 'text'],
    ['MatchContains', 'ID/класс содержит', 'textarea'],
    ['ChestName', 'Название сундука', 'text']
  ],
  ZoneRobotRules: [
    ['Enabled', 'Вкл', 'checkbox'],
    ['Name', 'Название правила', 'text'],
    ['Zone', 'Зона/сектор', 'text'],
    ['ScheduleTimesOn', 'Включить в HH:mm', 'text'],
    ['ScheduleTimesOff', 'Выключить в HH:mm', 'text'],
    ['CommandTemplateOn', 'Команда включения', 'textarea'],
    ['CommandTemplateOff', 'Команда выключения', 'textarea'],
    ['AnnouncementOn', 'Объявление включения', 'text'],
    ['AnnouncementOff', 'Объявление выключения', 'text']
  ],
  QuestRules: [
    ['Enabled', 'Вкл', 'checkbox'],
    ['Alias', 'Команда квеста', 'text'],
    ['Title', 'Название', 'text'],
    ['Mode', 'Режим', 'select:Claim|Manual|TurnInHands'],
    ['Description', 'Описание', 'textarea'],
    ['TargetItemId', 'Целевой предмет', 'text', 'itemCatalog'],
    ['TargetCount', 'Кол-во цели', 'number'],
    ['RewardMoney', 'Награда деньги', 'number'],
    ['RewardGold', 'Награда золото', 'number'],
    ['RewardFame', 'Награда слава', 'number'],
    ['RewardItemsText', 'Предметы награды', 'textarea'],
    ['SuccessMessage', 'Сообщение игроку', 'text'],
    ['Announcement', 'Объявление', 'text']
  ],
  Items: [
    ['ItemId', 'ID предмета', 'text', 'itemCatalog'],
    ['Quantity', 'Количество', 'number']
  ],
  BattlepassRewards: [
    ['Enabled', 'Включено', 'checkbox'],
    ['Day', 'День', 'number'],
    ['MoneyAmount', 'Деньги', 'number'],
    ['GoldAmount', 'Золото', 'number'],
    ['FameAmount', 'Слава', 'number'],
    ['ItemsText', 'Предметы', 'textarea'],
    ['Message', 'Сообщение игроку', 'text']
  ],
  Members: [
    ['Enabled', 'Активен', 'checkbox'],
    ['SteamId', 'SteamID', 'text'],
    ['Name', 'Ник / заметка', 'text'],
    ['Tier', 'Уровень VIP', 'text'],
    ['ExpiresAtUtc', 'Действует до UTC', 'text'],
    ['Permissions', 'Доп. права игрока', 'line-list'],
    ['Note', 'Заметка', 'text']
  ],
  Vehicles: [
    ['Alias', 'Команда', 'text'],
    ['DisplayName', 'Название', 'text'],
    ['AssetName', 'ID транспорта', 'text', 'vehicleCatalog'],
    ['PricePer10Minutes', 'Цена / 10 мин', 'number'],
    ['InitialCharge', 'Стартовая цена', 'number'],
    ['DefaultMinutes', 'Время, мин', 'number'],
    ['MaxMinutes', 'Макс. минут', 'number'],
    ['MissingVehiclePenalty', 'Штраф за продажу/потерю', 'number']
  ],
  WelcomeVehicles: [
    ['Enabled', 'Включено', 'checkbox'],
    ['Alias', 'Внутренний ID', 'text'],
    ['DisplayName', 'Название', 'text'],
    ['AssetName', 'ID транспорта', 'text', 'vehicleCatalog'],
    ['Minutes', 'Время аренды, мин', 'number'],
    ['MissingVehiclePenalty', 'Штраф за продажу/потерю', 'number'],
    ['SuccessMessage', 'Сообщение игроку', 'text']
  ],
  Outposts: [
    ['DisplayName', 'Название', 'text'],
    ['CommandAlias', 'Команда', 'text'],
    ['ArrivalPoint.0', 'Точка X', 'number'],
    ['ArrivalPoint.1', 'Точка Y', 'number'],
    ['ArrivalPoint.2', 'Точка Z', 'number'],
    ['Price', 'Цена маршрута', 'number']
  ],
  Rules: [
    ['Enabled', 'Вкл', 'checkbox'],
    ['MatchOfferId', 'ID предложения', 'text'],
    ['MatchBucketId', 'ID строки корзины', 'text'],
    ['MatchProductId', 'ID товара / SKU', 'text'],
    ['MatchItemId', 'ID объекта магазина', 'text'],
    ['MatchTitleContains', 'Название содержит', 'text'],
    ['MatchCommandContains', 'Команда содержит', 'text'],
    ['DeliveryMode', 'Режим', 'select:SpawnItem|Vehicle|CargoDropPlayer|Vip|Money|Gold|Fame|Attributes|Skill|AllSkills|PlayerCommand|ServerCommand'],
    ['DeliveryLabel', 'Метка', 'text'],
    ['ItemId', 'ID предмета', 'text', 'itemCatalog'],
    ['Quantity', 'Кол-во', 'number'],
    ['VehicleAsset', 'ID транспорта', 'text', 'vehicleCatalog'],
    ['DurationDays', 'Дней VIP', 'number'],
    ['Tier', 'Уровень VIP', 'text'],
    ['Amount', 'Сумма', 'number'],
    ['SkillName', 'Навык', 'text', 'skillCatalog'],
    ['SkillLevel', 'Уровень', 'number'],
    ['SkillExperience', 'Опыт навыка', 'number'],
    ['Strength', 'Сила', 'number'],
    ['Constitution', 'Телосложение', 'number'],
    ['Dexterity', 'Ловкость', 'number'],
    ['Intelligence', 'Интеллект', 'number'],
    ['CommandTemplate', 'Шаблон команды', 'textarea'],
    ['SuccessMessage', 'Сообщение', 'text']
  ],
  UpgradeRules: [
    ['Enabled', 'Вкл', 'checkbox'],
    ['Alias', 'Команда апгрейда', 'text'],
    ['DisplayName', 'Название', 'text'],
    ['SourceItemId', 'Предмет в руках', 'text', 'itemCatalog'],
    ['ResultItemId', 'Что выдать', 'text', 'itemCatalog'],
    ['Weight', 'Вес нового предмета, кг', 'number'],
    ['Health', 'HP / состояние, %', 'number'],
    ['Uses', 'Uses / заряды', 'number'],
    ['Dirtiness', 'Загрязнение, %', 'number'],
    ['AmmoCount', 'Патроны / заряд', 'number'],
    ['CashValue', 'Сумма наличных', 'number'],
    ['KeyCardSector', 'Сектор ключ-карты', 'text'],
    ['CostMoney', 'Цена деньги', 'number'],
    ['CostGold', 'Цена золото', 'number'],
    ['CostFame', 'Цена слава', 'number'],
    ['RequiredCostItemId', 'Предмет-стоимость', 'text', 'itemCatalog'],
    ['RequiredCostItemQuantity', 'Кол-во предмета-стоимости', 'number'],
    ['SuccessMessage', 'Сообщение', 'text']
  ],
  SkillPresets: [
    ['Name', 'Название', 'text'],
    ['Skill', 'Навык', 'text'],
    ['Level', 'Уровень', 'number'],
    ['Experience', 'Опыт', 'number']
  ],
  Packs: [
    ['Name', 'Название', 'text'],
    ['Enabled', 'Вкл', 'checkbox'],
    ['Description', 'Описание', 'text'],
    ['CooldownHours', 'Кулдаун, ч', 'number'],
    ['Attributes.Strength', 'Сила', 'number'],
    ['Attributes.Constitution', 'Телосложение', 'number'],
    ['Attributes.Dexterity', 'Ловкость', 'number'],
    ['Attributes.Intelligence', 'Интеллект', 'number'],
    ['Actions.0.Value', 'Команда 1', 'text'],
    ['SuccessMessage', 'Сообщение', 'text']
  ]
};
arrayEditors.items = arrayEditors.Items;
arrayEditors.members = arrayEditors.Members;
arrayEditors.vehicles = arrayEditors.Vehicles;
arrayEditors.rules = arrayEditors.Rules;
arrayEditors.upgradeRules = arrayEditors.UpgradeRules;
arrayEditors.baseLootRules = arrayEditors.BaseLootRules;
arrayEditors.zoneRobotRules = arrayEditors.ZoneRobotRules;
arrayEditors.quests = arrayEditors.QuestRules;
arrayEditors.jobs = arrayEditors.Jobs;
arrayEditors.points = arrayEditors.Points;
arrayEditors.itemSets = arrayEditors.ItemSets;
arrayEditors.itemsets = arrayEditors.ItemSets;
arrayEditors.outposts = arrayEditors.Outposts;
arrayEditors.skillPresets = arrayEditors.SkillPresets;
arrayEditors.warningMinutes = null;

function setModuleDraftText() {
  els.moduleConfig.value = JSON.stringify(state.moduleDraft || {}, null, 2);
}

function clonePlain(value) {
  return JSON.parse(JSON.stringify(value == null ? {} : value));
}

function resetSimpleModuleItemEditor() {
  state.simpleModuleEditIndex = null;
  state.simpleModuleEditArrayKey = null;
  state.simpleModuleEditDraft = null;
  state.simpleModuleEditIsNew = false;
}

function simpleModuleArrayMatch(profile, key, config = state.moduleDraft) {
  if (!profile || !key) return null;
  const low = String(key).toLowerCase();
  if (low === simpleModuleArrayKey(config || {}, profile).toLowerCase()) return { tab: 'items' };
  if (simpleModuleHasVipItems(profile) && low === simpleModuleVipArrayKey(config || {}, profile).toLowerCase()) return { tab: 'vip-items' };
  const extra = simpleModuleExtraArrays(profile).find(entry => low === simpleModuleExtraArrayKey(config || {}, entry).toLowerCase());
  return extra ? { tab: extra.tab } : null;
}

function beginSimpleModuleCreate(arrayKey) {
  const profile = simpleModuleProfile();
  const match = simpleModuleArrayMatch(profile, arrayKey);
  if (!profile || !match) return false;
  if (!state.moduleDraft || typeof state.moduleDraft !== 'object') state.moduleDraft = {};
  state.simpleModuleTabs[profile.key] = match.tab;
  state.simpleModuleEditArrayKey = arrayKey;
  state.simpleModuleEditIndex = -1;
  state.simpleModuleEditDraft = clonePlain(defaultArrayItem(arrayKey, state.selectedModule));
  state.simpleModuleEditIsNew = true;
  state.simpleSettingEditPath = null;
  renderModuleFields(state.moduleDraft);
  return true;
}

function beginSimpleModuleEdit(arrayKey, index) {
  const profile = simpleModuleProfile();
  const match = simpleModuleArrayMatch(profile, arrayKey);
  if (!profile || !match || !state.moduleDraft || !Array.isArray(state.moduleDraft[arrayKey]) || !state.moduleDraft[arrayKey][index]) return false;
  state.simpleModuleTabs[profile.key] = match.tab;
  state.simpleModuleEditArrayKey = arrayKey;
  state.simpleModuleEditIndex = index;
  state.simpleModuleEditDraft = clonePlain(state.moduleDraft[arrayKey][index]);
  state.simpleModuleEditIsNew = false;
  state.simpleSettingEditPath = null;
  renderModuleFields(state.moduleDraft);
  return true;
}

function commitSimpleModuleItemDraft(options = {}) {
  const key = state.simpleModuleEditArrayKey;
  const draft = state.simpleModuleEditDraft;
  if (!key || !draft || !state.moduleDraft) return false;
  if (!Array.isArray(state.moduleDraft[key])) state.moduleDraft[key] = [];
  const row = clonePlain(draft);
  if (state.simpleModuleEditIsNew) {
    state.moduleDraft[key].push(row);
    state.simpleModuleEditIndex = state.moduleDraft[key].length - 1;
    state.simpleModuleEditIsNew = false;
  } else if (Number.isInteger(state.simpleModuleEditIndex) && state.moduleDraft[key][state.simpleModuleEditIndex]) {
    state.moduleDraft[key][state.simpleModuleEditIndex] = row;
  } else {
    return false;
  }
  setModuleDraftText();
  if (options.message && els.quickResult) els.quickResult.textContent = 'Изменение добавлено в черновик. Нажмите «Сохранить изменения», чтобы записать конфиг.';
  if (options.reset) resetSimpleModuleItemEditor();
  if (options.render) renderModuleFields(state.moduleDraft);
  return true;
}

async function saveSimpleModuleItemEditor(options = {}) {
  const committed = commitSimpleModuleItemDraft({ message: !options.persist, reset: true, render: true });
  if (committed && options.persist) {
    return saveSelectedModule();
  }
  return { ok: committed, message: committed ? 'Изменение добавлено в черновик.' : 'Нет открытого изменения.' };
}

function defaultArrayItem(key, moduleKey = state.selectedModule) {
  const normalized = key.toLowerCase();
  const module = String(moduleKey || '').toLowerCase();
  if (module === 'item-upgrade' && normalized === 'rules') {
    return { Enabled: false, Alias: 'barrett-light', DisplayName: 'Лёгкий Barrett', SourceItemId: 'Weapon_M82A1', ResultItemId: 'Weapon_M82A1', Weight: 1, Health: 0, Uses: 0, Dirtiness: 0, AmmoCount: 0, CashValue: 0, KeyCardSector: '', CostMoney: 10000, CostGold: 0, CostFame: 0, RequiredCostItemId: '', RequiredCostItemQuantity: 0, SuccessMessage: 'Предмет улучшен. Заберите новую версию рядом с собой.' };
  }
  if (module === 'zone-robot-schedule' && normalized === 'rules') {
    return { Enabled: false, Name: 'Новое правило зоны', Zone: 'B2', ScheduleTimesOn: '21:00', ScheduleTimesOff: '06:00', CommandTemplateOn: 'Announce Роботы включены в зоне {Zone}.', CommandTemplateOff: 'Announce Роботы отключены в зоне {Zone}.', AnnouncementOn: '', AnnouncementOff: '' };
  }
  if (module === 'base-loot-collector' && normalized === 'rules') {
    return { Enabled: true, Name: 'Новое правило', MatchContains: 'weapon;ammo;food', ChestName: 'Loot' };
  }
  if (module === 'panel-quests' && normalized === 'quests') {
    return { Enabled: true, Alias: 'quest', Title: 'Новый квест', Mode: 'Claim', Description: 'Описание квеста.', TargetItemId: '', TargetCount: 0, RewardMoney: 0, RewardGold: 0, RewardFame: 0, RewardItemsText: 'Apple_2|1', SuccessMessage: 'Квест выполнен: {title}.', Announcement: '' };
  }
  if (normalized === 'items') return key === 'items' ? { itemId: 'Apple_2', quantity: 1 } : { ItemId: 'Apple_2', Quantity: 1 };
  if (normalized === 'vipitems') return key === 'vipitems' ? { itemId: 'Apple_2', quantity: 1 } : { ItemId: 'Apple_2', Quantity: 1 };
  if (normalized === 'rewards' || normalized === 'viprewards') {
    const vip = normalized === 'viprewards';
    return key === normalized
      ? { enabled: true, day: 1, moneyAmount: vip ? 5000 : 3000, goldAmount: 0, fameAmount: 0, itemsText: vip ? 'Apple_2|4' : 'Apple_2|2', message: vip ? 'VIP Battlepass награда {day}/{maxDays} получена.' : 'Battlepass награда {day}/{maxDays} получена.' }
      : { Enabled: true, Day: 1, MoneyAmount: vip ? 5000 : 3000, GoldAmount: 0, FameAmount: 0, ItemsText: vip ? 'Apple_2|4' : 'Apple_2|2', Message: vip ? 'VIP Battlepass награда {day}/{maxDays} получена.' : 'Battlepass награда {day}/{maxDays} получена.' };
  }
  if (normalized === 'members') {
    const expires = new Date(Date.now() + 30 * 86400 * 1000).toISOString().replace(/\.\d{3}Z$/, 'Z');
    return key === 'members'
      ? { enabled: true, steamId: '', name: '', tier: 'vip', expiresAtUtc: expires, permissions: [], note: '' }
      : { Enabled: true, SteamId: '', Name: '', Tier: 'vip', ExpiresAtUtc: expires, Permissions: [], Note: '' };
  }
  if (normalized === 'vehicles' || normalized === 'vipvehicles') return key === normalized ? { alias: 'vehicle', displayName: 'Транспорт', assetName: 'BPC_WolfsWagen', pricePer10Minutes: 0, initialCharge: 0, defaultMinutes: 10, maxMinutes: 60, missingVehiclePenalty: 0 } : { Alias: 'vehicle', DisplayName: 'Транспорт', AssetName: 'BPC_WolfsWagen', PricePer10Minutes: 0, InitialCharge: 0, DefaultMinutes: 10, MaxMinutes: 60, MissingVehiclePenalty: 0 };
  if (normalized === 'rentalvehicles' || normalized === 'welcomevehicles' || normalized === 'startervehicles') return key === normalized ? { enabled: true, alias: 'starter-vehicle', displayName: 'Стартовый транспорт', assetName: 'BPC_WolfsWagen', minutes: 1440, missingVehiclePenalty: 25000, successMessage: 'Транспорт стартового набора выдан на 24 часа.' } : { Enabled: true, Alias: 'starter-vehicle', DisplayName: 'Стартовый транспорт', AssetName: 'BPC_WolfsWagen', Minutes: 1440, MissingVehiclePenalty: 25000, SuccessMessage: 'Транспорт стартового набора выдан на 24 часа.' };
  if (normalized === 'jobs') return { Enabled: true, Name: 'Новое задание', Mode: 'Command', ScheduleTimes: '12:00', IntervalMinutes: 60, RunOnStartup: false, PointGroup: '', PointCount: 1, ItemSet: '', WorldEventClass: '', CommandTemplate: 'Announce Плановое сообщение сервера.', Announcement: '', MaxItemsPerRun: 30 };
  if (normalized === 'points') return { Name: 'Новая точка', Group: 'event', X: 0, Y: 0, Z: 50, Radius: 0 };
  if (normalized === 'itemsets') return { Name: 'new-set', Weight: 1, ItemsText: 'Apple_2|1' };
  if (normalized === 'outposts' || normalized === 'points') return key === 'points' ? { displayName: 'Новый маршрут', commandAlias: 'route', arrivalPoint: [0, 0, 50], price: 0 } : { DisplayName: 'Новый маршрут', CommandAlias: 'route', CenterZone: [0, 0], ArrivalPoint: [0, 0, 50], Price: 0 };
  if (normalized === 'rules') {
    const gameStoresFields = module === 'gamestores-shop'
      ? { MatchBucketId: '', MatchProductId: '', MatchCommandContains: '' }
      : {};
    const gameStoresFieldsLower = module === 'gamestores-shop'
      ? { matchBucketId: '', matchProductId: '', matchCommandContains: '' }
      : {};
    return key === 'rules'
      ? Object.assign({ enabled: true, matchOfferId: '', matchItemId: '', matchTitleContains: '', deliveryMode: 'SpawnItem', deliveryLabel: 'Выдача', itemId: 'Apple_2', items: [{ itemId: 'Apple_2', quantity: 1 }], quantity: 1, vehicleAsset: '', durationDays: 30, tier: 'vip', amount: 0, successMessage: 'Покупка выдана.' }, gameStoresFieldsLower)
      : Object.assign({ Enabled: true, MatchOfferId: '', MatchItemId: '', MatchTitleContains: '', DeliveryMode: 'SpawnItem', DeliveryLabel: 'Выдача', ItemId: 'Apple_2', Items: [{ ItemId: 'Apple_2', Quantity: 1 }], VehicleAsset: '', DurationDays: 30, Tier: 'vip', Quantity: 1, Amount: 0, SkillName: '', SkillLevel: 0, SkillExperience: 0, Strength: 0, Constitution: 0, Dexterity: 0, Intelligence: 0, CommandTemplate: '', SuccessMessage: 'Покупка выдана.' }, gameStoresFields);
  }
  if (normalized === 'skillpresets') return { Name: 'Навык', Skill: 'Survival', Level: 1, Experience: 0 };
  if (normalized === 'packs') return { Name: 'fullstats', Enabled: true, Description: 'Быстрый пакет персонажа', CooldownHours: 0, Attributes: { Strength: 8, Constitution: 8, Dexterity: 8, Intelligence: 8 }, Actions: [{ Type: 'PlayerCommand', Value: 'SetAttributes 8 8 8 8', StopOnFailure: true }], Skills: [], SuccessMessage: 'Атрибуты персонажа обновлены.' };
  return {};
}

function getNestedValue(obj, path) {
  const parts = String(path).split('.');
  let cur = obj;
  for (const part of parts) {
    if (cur == null) return '';
    const actual = Object.prototype.hasOwnProperty.call(cur, part)
      ? part
      : Object.keys(cur).find(key => key.toLowerCase() === part.toLowerCase());
    cur = actual === undefined ? undefined : cur[actual];
  }
  return cur ?? '';
}

function setNestedValue(obj, path, value) {
  const parts = String(path).split('.');
  let cur = obj;
  for (let i = 0; i < parts.length - 1; i += 1) {
    const part = Object.keys(cur).find(key => key.toLowerCase() === parts[i].toLowerCase()) || parts[i];
    const next = parts[i + 1];
    if (cur[part] == null) cur[part] = /^\d+$/.test(next) ? [] : {};
    cur = cur[part];
  }
  const last = Object.keys(cur).find(key => key.toLowerCase() === parts[parts.length - 1].toLowerCase()) || parts[parts.length - 1];
  cur[last] = value;
}

function renderArrayInput(arrayKey, index, field, label, kind, listId, row) {
  const value = getNestedValue(row, field);
  const base = `data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="${escapeAttr(field)}" data-array-kind="${escapeAttr(kind || '')}"`;
  const fieldLower = String(field || '').toLowerCase();
  const arrayLower = String(arrayKey || '').toLowerCase();
  if ((fieldLower === 'itemstext' || fieldLower === 'rewarditemstext' || fieldLower === 'itemsspec') &&
      !['itemsets'].includes(arrayLower)) {
    return renderBattlepassItemsBuilder(arrayKey, index, field, label, row || {});
  }
  if (kind === 'checkbox') {
    return `<div class="mini-check toggle-line"><span>${escapeHtml(label)}</span>${renderToggleInput(base, !!value)}</div>`;
  }
  if (kind.startsWith('select:')) {
    const options = kind.slice(7).split('|').map(opt => `<option value="${escapeAttr(opt)}" ${String(value) === opt ? 'selected' : ''}>${escapeHtml(selectOptionLabel(opt))}</option>`).join('');
    return `<label><span>${escapeHtml(label)}</span><select ${base}>${options}</select></label>`;
  }
  if (kind === 'textarea') {
    return `<label class="wide"><span>${escapeHtml(label)}</span><textarea ${base}>${escapeHtml(value)}</textarea></label>`;
  }
  if (kind === 'line-list' || kind === 'number-list') {
    const text = Array.isArray(value) ? value.join(kind === 'number-list' ? ', ' : '\n') : String(value || '');
    return `<label class="wide"><span>${escapeHtml(label)}</span><textarea ${base}>${escapeHtml(text)}</textarea></label>`;
  }
  const inputKind = kind === 'number' ? 'number' : 'text';
  const list = listId ? ` list="${escapeAttr(listId)}"` : '';
  return `<label><span>${escapeHtml(label)}</span><input ${base} type="${inputKind}"${list} value="${escapeAttr(value)}" /></label>`;
}

function battlepassItemsTextKey(row) {
  return Object.keys(row || {}).find(key => key.toLowerCase() === 'itemstext') ||
    Object.keys(row || {}).find(key => key.toLowerCase() === 'itemsspec') ||
    'ItemsText';
}

function parseBattlepassItemsText(text) {
  return String(text || '')
    .split(/[;\r\n]+/)
    .map(part => part.trim())
    .filter(Boolean)
    .map(part => {
      const bits = part.split('|');
      const itemId = String(bits[0] || '').trim();
      const quantity = Math.max(1, Math.floor(Number(bits[1] || 1) || 1));
      return itemId ? { itemId, quantity } : null;
    })
    .filter(Boolean);
}

function formatBattlepassItemsText(items) {
  return (items || [])
    .map(item => {
      const itemId = String(item && item.itemId || '').trim();
      const quantity = Math.max(1, Math.floor(Number(item && item.quantity || 1) || 1));
      return itemId ? `${itemId}|${quantity}` : '';
    })
    .filter(Boolean)
    .join(';');
}

function battlepassItemCountLabel(count) {
  const n = Number(count) || 0;
  if (n <= 0) return 'без предметов';
  if (n === 1) return '1 предмет';
  if (n >= 2 && n <= 4) return `${n} предмета`;
  return `${n} предметов`;
}

function battlepassItemsSummary(text) {
  return battlepassItemCountLabel(parseBattlepassItemsText(text).length);
}

function battlepassRewardItems(row) {
  const key = battlepassItemsTextKey(row || {});
  return parseBattlepassItemsText(getNestedValue(row || {}, key) || '');
}

function setBattlepassRewardItems(row, items) {
  if (!row || typeof row !== 'object') return;
  row[battlepassItemsTextKey(row)] = formatBattlepassItemsText(items);
}

function renderBattlepassItemsBuilder(arrayKey, index, field, label, row) {
  const items = battlepassRewardItems(row || {});
  const rows = items.map((item, itemIndex) => {
    const itemId = item.itemId || '';
    const quantity = item.quantity || 1;
    return `<div class="battlepass-item-row" data-battlepass-item-row="${itemIndex}">
      ${catalogThumbHtml(itemId, 'item', { className: 'battlepass-item-thumb', attrs: 'data-battlepass-item-thumb="true"', allowGenerated: true })}
      <label class="battlepass-item-id"><span>Предмет</span><input data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="${escapeAttr(field)}" data-battlepass-item-field="ItemId" data-battlepass-item-index="${itemIndex}" list="itemCatalog" value="${escapeAttr(itemId)}" placeholder="Apple_2" /></label>
      <label class="battlepass-item-qty"><span>Кол-во</span><input data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="${escapeAttr(field)}" data-battlepass-item-field="Quantity" data-battlepass-item-index="${itemIndex}" type="number" min="1" step="1" value="${escapeAttr(quantity)}" /></label>
      <button type="button" class="mini-action danger battlepass-item-remove" data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="${escapeAttr(field)}" data-battlepass-item-remove="${itemIndex}" title="Удалить предмет">Удалить</button>
    </div>`;
  }).join('');
  return `<div class="field wide battlepass-items-builder" data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="${escapeAttr(field)}">
    <div class="battlepass-items-head">
      <span>${escapeHtml(label || 'Предметы')}</span>
      <button type="button" class="mini-action primary" data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="${escapeAttr(field)}" data-battlepass-item-add="true">Добавить предмет</button>
    </div>
    <div class="battlepass-item-list">
      ${rows || '<div class="muted-line battlepass-items-empty">Предметов нет.</div>'}
    </div>
  </div>`;
}

const wargmRuleModeMeta = {
  SpawnItem: {
    label: 'Предметы',
    help: 'Один товар Wargm может выдавать целый сет. Добавь несколько строк в набор ниже: каждая строка будет выдана после одной покупки.',
    fields: [['ItemId', 'ID предмета', 'text', 'itemCatalog'], ['Quantity', 'Кол-во', 'number']]
  },
  Vehicle: {
    label: 'Транспорт',
    help: 'Спавнит транспорт рядом с игроком через безопасную очередь.',
    fields: [['VehicleAsset', 'ID транспорта', 'text', 'vehicleCatalog'], ['Quantity', 'Кол-во', 'number']]
  },
  CargoDropPlayer: {
    label: 'Cargo drop',
    help: 'Вызывает SCUM cargo drop по live-координатам игрока через ScheduleWorldEvent BP_CargoDropEvent X= Y= Z=.',
    fields: []
  },
  Vip: {
    label: 'VIP',
    help: 'Активирует VIP-привилегию игроку на указанное количество дней. Срок продлевается от текущего активного VIP, если это включено в VIP-модуле.',
    fields: [['DurationDays', 'Дней VIP', 'number'], ['Tier', 'Уровень VIP', 'text']]
  },
  Money: {
    label: 'Деньги',
    help: 'Начисляет обычную валюту игроку. Для списания укажи отрицательное число.',
    fields: [['Amount', 'Сумма', 'number']]
  },
  Gold: {
    label: 'Золото',
    help: 'Начисляет золото игроку через тот же безопасный маршрут баланса. Для списания укажи отрицательное число.',
    fields: [['Amount', 'Сумма золота', 'number']]
  },
  Fame: {
    label: 'Очки славы',
    help: 'Начисляет очки славы поверх текущей суммы. Для списания укажи отрицательное число.',
    fields: [['Amount', 'Очки славы', 'number']]
  },
  Attributes: {
    label: 'Атрибуты',
    help: 'Выставляет STR / CON / DEX / INT выбранному игроку.',
    fields: [['Strength', 'Сила', 'number'], ['Constitution', 'Телосложение', 'number'], ['Dexterity', 'Ловкость', 'number'], ['Intelligence', 'Интеллект', 'number']]
  },
  Skill: {
    label: 'Навык',
    help: 'Выставляет один навык. Названия: Handgun, Melee Weapons, Rifles и другие из каталога.',
    fields: [['SkillName', 'Навык', 'text', 'skillCatalog'], ['SkillLevel', 'Уровень', 'number'], ['SkillExperience', 'Опыт', 'number']]
  },
  AllSkills: {
    label: 'Все навыки',
    help: 'Выставляет весь актуальный список навыков SCUM. Для максимума укажи уровень 4.',
    fields: [['SkillLevel', 'Уровень', 'number'], ['SkillExperience', 'Опыт', 'number']]
  },
  PlayerCommand: {
    label: 'Команда игрока',
    help: 'Редкий режим. Используй только проверенные команды.',
    fields: [['CommandTemplate', 'Шаблон команды', 'textarea']]
  },
  ServerCommand: {
    label: 'Команда сервера',
    help: 'Редкий режим. Используй только проверенные команды.',
    fields: [['CommandTemplate', 'Шаблон команды', 'textarea']]
  }
};

const wargmRuleModeAliases = {
  item: 'SpawnItem',
  items: 'SpawnItem',
  spawnitem: 'SpawnItem',
  vehicle: 'Vehicle',
  spawnvehicle: 'Vehicle',
  cargodrop: 'CargoDropPlayer',
  cargodropplayer: 'CargoDropPlayer',
  airdrop: 'CargoDropPlayer',
  scheduledcargodrop: 'CargoDropPlayer',
  vip: 'Vip',
  vipprivilege: 'Vip',
  vipprivileges: 'Vip',
  subscription: 'Vip',
  money: 'Money',
  currency: 'Money',
  balance: 'Money',
  gold: 'Gold',
  fame: 'Fame',
  famepoint: 'Fame',
  famepoints: 'Fame',
  changefame: 'Fame',
  changefamepoints: 'Fame',
  setfame: 'Fame',
  setfamepoints: 'Fame',
  attributes: 'Attributes',
  attribute: 'Attributes',
  stats: 'Attributes',
  skill: 'Skill',
  setskill: 'Skill',
  allskills: 'AllSkills',
  allskill: 'AllSkills',
  skillpack: 'AllSkills',
  skillspack: 'AllSkills',
  playercommand: 'PlayerCommand',
  servercommand: 'ServerCommand',
  command: 'PlayerCommand'
};

function wargmRuleValue(row, key, fallback = '') {
  if (!row) return fallback;
  const actual = Object.keys(row).find(item => item.toLowerCase() === key.toLowerCase());
  if (!actual) return fallback;
  const value = row[actual];
  return value == null ? fallback : value;
}

function wargmRuleMode(row) {
  const uiMode = String(wargmRuleValue(row, 'UiDeliveryMode', '') || '');
  if (wargmRuleModeMeta[uiMode]) return uiMode;
  const mode = String(wargmRuleValue(row, 'DeliveryMode', 'SpawnItem') || 'SpawnItem');
  if (wargmRuleModeMeta[mode]) return mode;
  const commandTemplate = String(wargmRuleValue(row, 'CommandTemplate', '') || '').toLowerCase();
  if (commandTemplate.includes('changefamepoints') || commandTemplate.includes('setfamepoints')) return 'Fame';
  const normalized = mode.toLowerCase().replace(/[\s_.-]+/g, '');
  return wargmRuleModeAliases[normalized] || 'SpawnItem';
}

function renderWargmRuleInput(arrayKey, index, row, field, label, kind, listId) {
  if (field === 'DeliveryMode') {
    return renderArrayInput(arrayKey, index, field, label, kind, listId, Object.assign({}, row || {}, { DeliveryMode: wargmRuleMode(row || {}) }));
  }
  return renderArrayInput(arrayKey, index, field, label, kind, listId, row || {});
}

function isExternalShopModule(key = state.selectedModule) {
  const normalized = String(key || '').toLowerCase();
  return normalized === 'wargm-shop' || normalized === 'gamestores-shop';
}

function selectedShopModuleName(key = state.selectedModule) {
  return String(key || '').toLowerCase() === 'gamestores-shop' ? 'GameStores' : 'Wargm';
}

function renderWargmRuleItems(arrayKey, index, row) {
  const items = Array.isArray(row && row.Items) ? row.Items : (Array.isArray(row && row.items) ? row.items : []);
  const total = items.reduce((sum, item) => sum + Math.max(1, Number(item.Quantity || item.quantity || 1)), 0);
  const rows = items.map((item, itemIndex) => {
    const itemId = String(item.ItemId || item.itemId || '').trim();
    return `<div class="rule-item-row has-thumb">
      ${catalogThumbHtml(itemId, 'item', { className: 'rule-item-thumb', attrs: 'data-rule-item-thumb="true"', allowGenerated: true })}
      <span class="rule-item-index">${itemIndex + 1}</span>
      <label><span>ID предмета</span><input data-rule-item-field="ItemId" data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-rule-item-index="${itemIndex}" list="itemCatalog" value="${escapeAttr(itemId)}" placeholder="Weapon_MK18" /></label>
      <label class="qty"><span>Кол-во</span><input data-rule-item-field="Quantity" data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-rule-item-index="${itemIndex}" type="number" min="1" value="${escapeAttr(item.Quantity || item.quantity || 1)}" /></label>
      <button type="button" class="mini-action danger" data-wargm-rule-item-remove="${index}" data-rule-item-index="${itemIndex}">Удалить</button>
    </div>`;
  }).join('');
  return `<div class="wargm-rule-section nested-items">
    <div class="wargm-rule-section-head">
      <div>
        <h4>Набор предметов в одном товаре</h4>
        <p>${items.length ? `В сете ${items.length} строк, всего ${total} шт.` : 'Если список пустой, будет выдан одиночный предмет из поля выше.'}</p>
      </div>
      <div class="rule-item-actions">
        <button type="button" class="mini-action" data-wargm-rule-item-copy-single="${index}">Взять ID выше</button>
        <button type="button" class="mini-action primary" data-wargm-rule-item-add="${index}">Добавить строку</button>
      </div>
    </div>
    ${rows || '<div class="muted-line">Список пуст. Будет использован одиночный ID предмета выше.</div>'}
  </div>`;
}

function wargmRulePrimaryAsset(row, mode) {
  if (mode === 'Vehicle') return String(wargmRuleValue(row, 'VehicleAsset', '') || '').trim();
  if (mode !== 'SpawnItem') return '';
  const items = Array.isArray(row && row.Items) ? row.Items : (Array.isArray(row && row.items) ? row.items : []);
  const first = items.find(item => String(item.ItemId || item.itemId || '').trim());
  return String((first && (first.ItemId || first.itemId)) || wargmRuleValue(row, 'ItemId', '') || '').trim();
}

function renderWargmRuleVisual(row, mode, context = 'card') {
  const asset = wargmRulePrimaryAsset(row || {}, mode);
  if (mode === 'SpawnItem' && asset) return catalogThumbHtml(asset, 'item', { className: `wargm-rule-visual ${context}`, allowGenerated: true });
  if (mode === 'Vehicle' && asset) return catalogThumbHtml(asset, 'vehicle', { className: `wargm-rule-visual ${context}`, allowGenerated: true });
  const label = mode === 'CargoDropPlayer' ? 'CD' : mode === 'Vip' ? 'VIP' : mode === 'Money' ? '$' : mode === 'Gold' ? 'AU' : mode === 'Fame' ? 'FP' : mode === 'Skill' ? 'SK' : mode === 'AllSkills' ? 'AS' : mode === 'Attributes' ? 'AT' : 'CMD';
  return `<div class="wargm-item-thumb pack-item-thumb catalog-thumb wargm-rule-visual ${escapeAttr(context)} has-no-icon" aria-hidden="true"><span>${escapeHtml(label)}</span></div>`;
}

function renderWargmRuleDeliveryPreview(row, mode) {
  if (mode === 'SpawnItem') {
    const items = Array.isArray(row && row.Items) ? row.Items : (Array.isArray(row && row.items) ? row.items : []);
    const visible = items.length ? items : [{ ItemId: wargmRuleValue(row, 'ItemId', ''), Quantity: wargmRuleValue(row, 'Quantity', 1) }];
    const cards = visible
      .filter(item => String(item.ItemId || item.itemId || '').trim())
      .slice(0, 6)
      .map(item => {
        const itemId = String(item.ItemId || item.itemId || '').trim();
        const qty = Math.max(1, Number(item.Quantity || item.quantity || 1));
        const title = friendlyAssetName(itemId, 'item');
        return `<div class="wargm-delivery-preview-card">
          ${catalogThumbHtml(itemId, 'item', { className: 'wargm-preview-thumb', allowGenerated: true })}
          <div><b>${escapeHtml(itemId)}</b><span>${escapeHtml(title !== itemId ? title : '')}</span><small>x${qty}</small></div>
        </div>`;
      }).join('');
    return cards ? `<div class="wargm-delivery-preview">${cards}</div>` : '';
  }
  if (mode === 'Vehicle') {
    const vehicle = String(wargmRuleValue(row, 'VehicleAsset', '') || '').trim();
    if (!vehicle) return '';
    return `<div class="wargm-delivery-preview">
      <div class="wargm-delivery-preview-card vehicle">
        ${catalogThumbHtml(vehicle, 'vehicle', { className: 'wargm-preview-thumb', allowGenerated: true })}
        <div><b>${escapeHtml(vehicle)}</b><span>${escapeHtml(friendlyAssetName(vehicle, 'vehicle'))}</span><small>транспорт</small></div>
      </div>
    </div>`;
  }
  return '';
}

function renderWargmRuleSummary(row) {
  const offer = wargmRuleValue(row, 'MatchOfferId', '');
  const objectId = wargmRuleValue(row, 'MatchItemId', '') || wargmRuleValue(row, 'MatchObjectId', '') || wargmRuleValue(row, 'MatchProductId', '');
  const title = wargmRuleValue(row, 'MatchTitleContains', '');
  const label = wargmRuleValue(row, 'DeliveryLabel', '');
  return [offer ? `offer ${offer}` : '', objectId ? `объект ${objectId}` : '', title ? `название: ${title}` : '', label || 'без метки'].filter(Boolean).join(' · ');
}

function wargmRuleItemCount(row) {
  const items = Array.isArray(row && row.Items) ? row.Items : (Array.isArray(row && row.items) ? row.items : []);
  return items.length;
}

function wargmRuleDeliverySummary(row, mode) {
  if (mode === 'SpawnItem') {
    const count = wargmRuleItemCount(row || {});
    return count > 0 ? `${count} предметов` : `${wargmRuleValue(row, 'ItemId', 'предмет не указан')} x${wargmRuleValue(row, 'Quantity', 1) || 1}`;
  }
  if (mode === 'Vehicle') return wargmRuleValue(row, 'VehicleAsset', '') || 'транспорт не указан';
  if (mode === 'CargoDropPlayer') return 'cargo drop рядом с игроком';
  if (mode === 'Vip') return `VIP ${wargmRuleValue(row, 'Tier', 'vip') || 'vip'} на ${Number(wargmRuleValue(row, 'DurationDays', 30) || 30)} дн.`;
  if (mode === 'Money') return `баланс: ${Number(wargmRuleValue(row, 'Amount', 0) || 0)}`;
  if (mode === 'Gold') return `золото: ${Number(wargmRuleValue(row, 'Amount', 0) || 0)}`;
  if (mode === 'Fame') return `слава: ${Number(wargmRuleValue(row, 'Amount', 0) || 0)}`;
  if (mode === 'Skill') return `${wargmRuleValue(row, 'SkillName', 'навык')} ур. ${wargmRuleValue(row, 'SkillLevel', 0) || 0}`;
  if (mode === 'AllSkills') return `все навыки ур. ${wargmRuleValue(row, 'SkillLevel', 4) || 4}`;
  if (mode === 'Attributes') return `STR ${wargmRuleValue(row, 'Strength', 0) || 0} / CON ${wargmRuleValue(row, 'Constitution', 0) || 0} / DEX ${wargmRuleValue(row, 'Dexterity', 0) || 0} / INT ${wargmRuleValue(row, 'Intelligence', 0) || 0}`;
  return wargmRuleValue(row, 'CommandTemplate', '') || 'команда не указана';
}

function renderWargmRuleEditorBody(key, index, row) {
  const mode = wargmRuleMode(row || {});
  const meta = wargmRuleModeMeta[mode] || wargmRuleModeMeta.SpawnItem;
  const shopName = selectedShopModuleName();
  const isGameStores = shopName === 'GameStores';
  return `<div class="wargm-rule-layout">
        <section class="wargm-rule-section">
          <div class="wargm-rule-section-title">
            <div>
              <h4>1. Как найти покупку</h4>
            <p>ID/SKU берётся из карточки товара ${escapeHtml(shopName)}. Название можно оставить запасным вариантом.</p>
            </div>
            ${renderWargmRuleInput(key, index, row, 'Enabled', 'Правило включено', 'checkbox')}
          </div>
          <div class="array-grid wargm-setup-grid">
            ${renderWargmRuleInput(key, index, row, 'MatchOfferId', `ID предложения ${shopName}`, 'text')}
            ${renderWargmRuleInput(key, index, row, 'MatchItemId', `ID объекта ${shopName}`, 'text')}
            ${isGameStores ? renderWargmRuleInput(key, index, row, 'MatchBucketId', 'ID строки корзины', 'text') : ''}
            ${isGameStores ? renderWargmRuleInput(key, index, row, 'MatchProductId', 'ID товара / SKU', 'text') : ''}
            ${isGameStores ? renderWargmRuleInput(key, index, row, 'MatchCommandContains', 'Command содержит', 'text') : ''}
            ${renderWargmRuleInput(key, index, row, 'MatchTitleContains', 'Название содержит', 'text')}
            ${renderWargmRuleInput(key, index, row, 'DeliveryLabel', 'Метка для логов', 'text')}
          </div>
        </section>

        <section class="wargm-rule-section">
          <h4>2. Что выдать</h4>
          <p>${escapeHtml(meta.help)}</p>
          <div class="array-grid wargm-delivery-grid">
            ${renderWargmRuleInput(key, index, row, 'DeliveryMode', 'Тип выдачи', 'select:SpawnItem|Vehicle|CargoDropPlayer|Vip|Money|Gold|Fame|Attributes|Skill|AllSkills|PlayerCommand|ServerCommand')}
            ${meta.fields.map(([field, label, kind, listId]) => renderWargmRuleInput(key, index, row, field, label, kind, listId)).join('')}
          </div>
          ${renderWargmRuleDeliveryPreview(row || {}, mode)}
          ${mode === 'SpawnItem' ? renderWargmRuleItems(key, index, row || {}) : ''}
        </section>

        <section class="wargm-rule-section">
          <h4>3. Что написать игроку</h4>
          <div class="array-grid">
            ${renderWargmRuleInput(key, index, row, 'SuccessMessage', 'Сообщение после выдачи', 'text')}
          </div>
        </section>
      </div>`;
}

function renderWargmRuleModal(key, index, row) {
  const mode = wargmRuleMode(row || {});
  const meta = wargmRuleModeMeta[mode] || wargmRuleModeMeta.SpawnItem;
  return `<div class="wargm-rule-modal" data-wargm-rule-overlay="true" role="dialog" aria-modal="true">
    <div class="wargm-rule-modal-card">
      <div class="wargm-rule-modal-head">
        <div>
          <span class="wargm-modal-kicker">Настройка товара</span>
          <h3>Правило #${index + 1}</h3>
          <p>${escapeHtml(renderWargmRuleSummary(row || {}))}</p>
        </div>
        <div class="wargm-rule-modal-actions">
          <span class="wargm-mode-pill">${escapeHtml(meta.label)}</span>
          <button type="button" class="mini-action primary" data-wargm-rule-save="true">Сохранить товар</button>
          <button type="button" class="mini-action" data-wargm-rule-close="true">Закрыть</button>
        </div>
      </div>
      ${renderWargmRuleEditorBody(key, index, row || {})}
    </div>
  </div>`;
}

function renderWargmRulesEditor(key, value) {
  const shopName = selectedShopModuleName();
  const rows = Array.isArray(value) ? value : [];
  if (!Number.isInteger(state.wargmRuleEditIndex) || !rows[state.wargmRuleEditIndex]) state.wargmRuleEditIndex = null;
  const search = String(state.wargmRuleSearch || '').trim();
  const searchLower = search.toLowerCase();
  let visibleCount = 0;
  const body = rows.map((row, index) => {
    const mode = wargmRuleMode(row || {});
    const meta = wargmRuleModeMeta[mode] || wargmRuleModeMeta.SpawnItem;
    const enabled = wargmRuleValue(row || {}, 'Enabled', true) !== false;
    const offer = wargmRuleValue(row || {}, 'MatchOfferId', '') || 'offer не указан';
    const objectId = wargmRuleValue(row || {}, 'MatchItemId', '') || wargmRuleValue(row || {}, 'MatchObjectId', '') || wargmRuleValue(row || {}, 'MatchProductId', '');
    const title = wargmRuleValue(row || {}, 'MatchTitleContains', '') || (objectId ? `объект ${objectId}` : 'название не указано');
    const matchLine = [offer, objectId ? `объект ${objectId}` : ''].filter(Boolean).join(' / ') || 'матч не указан';
    const label = wargmRuleValue(row || {}, 'DeliveryLabel', '') || 'без метки';
    const deliveryLine = wargmRuleDeliverySummary(row || {}, mode);
    const searchText = [
      `правило ${index + 1}`,
      title,
      offer,
      objectId,
      matchLine,
      meta.label,
      deliveryLine,
      label
    ].join(' ').toLowerCase();
    const matches = !searchLower || searchText.includes(searchLower);
    if (matches) visibleCount += 1;
    return `<article class="wargm-rule-card ${state.wargmRuleEditIndex === index ? 'active' : ''}" data-wargm-rule-card="true" data-wargm-rule-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
      <div class="wargm-rule-card-main">
        ${renderWargmRuleVisual(row || {}, mode, 'card')}
        <div>
          <b>Правило #${index + 1}</b>
          <span>${escapeHtml(title)}</span>
        </div>
        <span class="wargm-rule-card-status ${enabled ? 'on' : 'off'}">${enabled ? 'вкл' : 'выкл'}</span>
      </div>
      <div class="wargm-rule-card-meta">
        <span>${escapeHtml(matchLine)}</span>
        <span>${escapeHtml(meta.label)}</span>
        <span>${escapeHtml(deliveryLine)}</span>
        <span>${escapeHtml(label)}</span>
      </div>
      <div class="wargm-rule-card-actions">
        <button type="button" class="mini-action primary" data-wargm-rule-open="${index}">Настроить</button>
        <button type="button" class="mini-action danger" data-array-remove="${escapeAttr(key)}" data-array-index="${index}">Удалить</button>
      </div>
    </article>`;
  }).join('');
  const selectedIndex = state.wargmRuleEditIndex;
  const selectedRow = selectedIndex !== null ? rows[selectedIndex] : null;
  const modal = selectedRow ? renderWargmRuleModal(key, selectedIndex, selectedRow || {}) : '';
  const emptyText = rows.length
    ? 'Поиск ничего не нашёл. Очисти строку поиска или измени запрос.'
    : `Правил пока нет. Нажми “Добавить правило” и укажи ID/SKU товара ${shopName}.`;
  return `<div class="field wide array-editor wargm-rules-editor">
    <div class="array-title">
      <div>
        <label>Правила ${escapeHtml(shopName)}</label>
        <p>Товары занимают всю рабочую область. Настройка открывается отдельным окном по кнопке “Настроить”.</p>
      </div>
      <button type="button" class="mini-action" data-array-add="${escapeAttr(key)}">Добавить правило</button>
    </div>
    <div class="wargm-rules-toolbar">
      <input class="wargm-rule-search" data-wargm-rule-search placeholder="Поиск товара: название, ID/SKU, выдача" value="${escapeAttr(search)}" />
      <span class="wargm-rule-search-count" data-wargm-rule-search-count>${visibleCount} / ${rows.length}</span>
    </div>
    <div class="wargm-rule-list">
      ${body}
      <div class="muted-line wargm-rule-search-empty" data-wargm-rule-search-empty ${visibleCount ? 'hidden' : ''}>${escapeHtml(emptyText)}</div>
    </div>
    ${modal}
  </div>`;
}

const schedulerModeMeta = {
  Airdrop: {
    label: 'Cargo drop',
    help: 'Запускает настоящий SCUM cargo drop через ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}.'
  },
  WorldEvent: {
    label: 'World event',
    help: 'Запускает выбранный класс world event через ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}.'
  },
  SpawnItems: {
    label: 'Лут в точку',
    help: 'Спавнит выбранный ItemSet прямо в точке карты через SpawnItem.'
  },
  Command: {
    label: 'Команда',
    help: 'Выполняет безопасную серверную команду по расписанию или интервалу.'
  }
};

const schedulerWorldEventClassSelect = 'select:BP_CargoDropEvent|BP_EncounterCargoDropEvent|BP_DeathmatchGameEvent|BP_TeamDeathmatchGameEvent|BP_CTFGameEvent|BP_DropZoneGameEvent';

function schedulerValue(row, key, fallback = '') {
  if (!row) return fallback;
  const actual = Object.keys(row).find(item => item.toLowerCase() === key.toLowerCase());
  return actual ? row[actual] : fallback;
}

function schedulerMode(row) {
  const mode = String(schedulerValue(row, 'Mode', 'Command') || 'Command');
  const actual = Object.keys(schedulerModeMeta).find(key => key.toLowerCase() === mode.toLowerCase());
  return actual || 'Command';
}

function renderToggleInput(attrs, checked) {
  return `<label class="toggle-switch">
    <input ${attrs} type="checkbox" ${checked ? 'checked' : ''} />
    <span class="slider"></span>
  </label>`;
}

function renderSchedulerTopControl(key, label, hint, kind = 'text') {
  const value = state.moduleDraft ? state.moduleDraft[key] : '';
  if (kind === 'checkbox') {
    return `<div class="setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
      <div class="setting-control">${renderToggleInput(`data-config-key="${escapeAttr(key)}"`, !!value)}</div>
    </div>`;
  }
  const inputKind = kind === 'number' ? 'number' : 'text';
  return `<div class="setting-row">
    <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
    <div class="setting-control"><input data-config-key="${escapeAttr(key)}" type="${inputKind}" value="${escapeAttr(value ?? '')}" /></div>
  </div>`;
}

function renderSchedulerArrayControl(arrayKey, index, field, label, hint, kind = 'text') {
  const row = state.moduleDraft && Array.isArray(state.moduleDraft[arrayKey]) ? state.moduleDraft[arrayKey][index] : {};
  const value = getNestedValue(row || {}, field);
  const base = `data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="${escapeAttr(field)}"`;
  if (kind === 'checkbox') {
    return `<div class="setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
      <div class="setting-control">${renderToggleInput(base, !!value)}</div>
    </div>`;
  }
  if (kind.startsWith('select:')) {
    const options = kind.slice(7).split('|').map(opt => {
      const modeLabel = schedulerModeMeta[opt] ? schedulerModeMeta[opt].label : selectOptionLabel(opt);
      return `<option value="${escapeAttr(opt)}" ${String(value) === opt ? 'selected' : ''}>${escapeHtml(modeLabel)}</option>`;
    }).join('');
    return `<div class="setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
      <div class="setting-control"><select ${base}>${options}</select></div>
    </div>`;
  }
  if (kind === 'textarea') {
    return `<div class="setting-row tall">
      <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
      <div class="setting-control"><textarea ${base}>${escapeHtml(value ?? '')}</textarea></div>
    </div>`;
  }
  const inputKind = kind === 'number' ? 'number' : 'text';
  return `<div class="setting-row">
    <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
    <div class="setting-control"><input ${base} type="${inputKind}" value="${escapeAttr(value ?? '')}" /></div>
  </div>`;
}

function renderSchedulerJobSummary(job) {
  const mode = schedulerMode(job || {});
  const times = schedulerValue(job, 'ScheduleTimes', '');
  const interval = Number(schedulerValue(job, 'IntervalMinutes', 0) || 0);
  if (times) return `${schedulerModeMeta[mode].label} · ${times}`;
  return `${schedulerModeMeta[mode].label} · каждые ${interval || 0} мин`;
}

function renderSchedulerJobsEditor(config) {
  const jobs = Array.isArray(config && config.Jobs) ? config.Jobs : [];
  const points = Array.isArray(config && config.Points) ? config.Points : [];
  const itemSets = Array.isArray(config && config.ItemSets) ? config.ItemSets : [];
  if (!Number.isInteger(state.schedulerJobIndex) || !jobs[state.schedulerJobIndex]) state.schedulerJobIndex = jobs.length ? 0 : null;
  const selectedIndex = state.schedulerJobIndex;
  const selectedJob = selectedIndex !== null ? jobs[selectedIndex] : null;
  const jobCards = jobs.map((job, index) => {
    const enabled = schedulerValue(job, 'Enabled', true) !== false;
    const mode = schedulerMode(job);
    return `<article class="scheduler-job-card ${selectedIndex === index ? 'active' : ''}" data-scheduler-job-open="${index}">
      <div class="scheduler-job-main">
        <b>${escapeHtml(schedulerValue(job, 'Name', `Задание #${index + 1}`))}</b>
        <span>${escapeHtml(renderSchedulerJobSummary(job))}</span>
      </div>
      <div class="scheduler-job-tags">
        <span>${escapeHtml(schedulerModeMeta[mode].label)}</span>
      </div>
      <button type="button" class="mini-action danger" data-array-remove="Jobs" data-array-index="${index}">Удалить</button>
    </article>`;
  }).join('');
  const workspace = selectedJob ? `<section class="scheduler-workspace">
      <div class="scheduler-workspace-head">
        <div>
          <span class="wargm-modal-kicker">Задание по расписанию</span>
          <h3>${escapeHtml(schedulerValue(selectedJob, 'Name', `Задание #${selectedIndex + 1}`))}</h3>
          <p>${escapeHtml(schedulerModeMeta[schedulerMode(selectedJob)].help)}</p>
        </div>
        <span class="wargm-mode-pill">${escapeHtml(schedulerModeMeta[schedulerMode(selectedJob)].label)}</span>
      </div>
      <div class="settings-grid scheduler-settings-grid">
        <div class="setting-group card">
          <h3><span class="icon">⚙</span> Основные</h3>
          <div class="setting-list">
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Enabled', 'Задание включено', 'Можно временно отключить без удаления.', 'checkbox')}
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Name', 'Название', 'Понятно видно в списке заданий.', 'text')}
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Mode', 'Тип задания', 'Cargo drop, world event, спавн набора или команда.', 'select:Airdrop|WorldEvent|SpawnItems|Command')}
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'RunOnStartup', 'Запуск при старте', 'Выполнить один раз после запуска сервера.', 'checkbox')}
          </div>
        </div>
        <div class="setting-group card">
          <h3><span class="icon">⌚</span> Время</h3>
          <div class="setting-list">
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'ScheduleTimes', 'Точное время', 'Например: 12:00, 18:30. Если пусто, работает интервал.', 'text')}
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'IntervalMinutes', 'Интервал, минут', 'Запасной режим, когда точное время не указано.', 'number')}
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'MaxItemsPerRun', 'Лимит предметов', 'Защита от случайного массового спавна.', 'number')}
          </div>
        </div>
        <div class="setting-group card">
          <h3><span class="icon">📍</span> Точка и набор</h3>
          <div class="setting-list">
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'PointGroup', 'Группа точек', 'Берёт случайную точку из списка ниже.', 'text')}
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'ItemSet', 'Набор дропа', 'Имя набора из блока ItemSets.', 'text')}
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Announcement', 'Сообщение игрокам', 'Можно использовать {Point} и {ItemSet}.', 'text')}
          </div>
        </div>
        <div class="setting-group card">
          <h3><span class="icon">⌁</span> Команда</h3>
          <div class="setting-list">
            ${renderSchedulerArrayControl('Jobs', selectedIndex, 'CommandTemplate', 'Шаблон команды', 'Поддерживает {X}, {Y}, {Z}, {Point}, {ItemSet}.', 'textarea')}
          </div>
        </div>
      </div>
    </section>` : `<section class="scheduler-workspace empty">
      <h3>Заданий пока нет</h3>
      <p>Добавь первое задание: объявление, cargo drop или спавн набора в точке карты.</p>
    </section>`;
  const pointRows = points.map((point, index) => `<div class="scheduler-resource-row">
    <div class="scheduler-resource-fields">
      ${renderArrayInput('Points', index, 'Name', 'Название', 'text', null, point)}
      ${renderArrayInput('Points', index, 'Group', 'Группа', 'text', null, point)}
      ${renderArrayInput('Points', index, 'X', 'X', 'number', null, point)}
      ${renderArrayInput('Points', index, 'Y', 'Y', 'number', null, point)}
      ${renderArrayInput('Points', index, 'Z', 'Z', 'number', null, point)}
      ${renderArrayInput('Points', index, 'Radius', 'Радиус', 'number', null, point)}
    </div>
    <button type="button" class="mini-action danger" data-array-remove="Points" data-array-index="${index}">Удалить</button>
  </div>`).join('');
  const setRows = itemSets.map((itemSet, index) => `<div class="scheduler-resource-row">
    <div class="scheduler-resource-fields itemset">
      ${renderArrayInput('ItemSets', index, 'Name', 'Название', 'text', null, itemSet)}
      ${renderArrayInput('ItemSets', index, 'Weight', 'Вес выбора', 'number', null, itemSet)}
      ${renderArrayInput('ItemSets', index, 'ItemsText', 'Предметы ItemId|Кол-во;...', 'textarea', null, itemSet)}
    </div>
    <button type="button" class="mini-action danger" data-array-remove="ItemSets" data-array-index="${index}">Удалить</button>
  </div>`).join('');
  return `<div class="field wide scheduler-editor">
    <div class="settings-header scheduler-header">
      <div class="header-info">
        <h2>Планировщик заданий</h2>
        <span class="sub-text">События по времени, интервалу, точкам карты и наборам предметов.</span>
      </div>
      <button type="button" class="btn primary" data-array-add="Jobs">Добавить задание</button>
    </div>
    <div class="scheduler-top-grid">
      <div class="setting-group card">
        <h3><span class="icon">✓</span> Базовые параметры</h3>
        <div class="setting-list">
          ${renderSchedulerTopControl('Enabled', 'Планировщик включен', 'Главный выключатель всех заданий.', 'checkbox')}
          ${renderSchedulerTopControl('Name', 'Название модуля', 'Отображается только в панели.', 'text')}
          ${renderSchedulerTopControl('MaxRunsPerTick', 'Макс. задач за тик', 'Оставь 1, чтобы не грузить сервер залпом.', 'number')}
        </div>
      </div>
      <div class="setting-group card scheduler-help">
        <h3><span class="icon">i</span> Как это работает</h3>
        <p>Укажи точное время в формате <b>12:00, 18:30</b>. Если время пустое, будет использоваться интервал в минутах. Для спавна наборов задай группу точек и ItemSet.</p>
      </div>
    </div>
    <div class="scheduler-shell">
      <aside class="scheduler-job-list">
        ${jobCards || '<div class="muted-line">Заданий пока нет.</div>'}
      </aside>
      ${workspace}
    </div>
    <div class="scheduler-resources-grid">
      <section class="setting-group card">
        <div class="scheduler-resource-head">
          <h3><span class="icon">◎</span> Точки карты</h3>
          <button type="button" class="mini-action" data-array-add="Points">Добавить точку</button>
        </div>
        ${pointRows || '<div class="muted-line">Точек пока нет.</div>'}
      </section>
      <section class="setting-group card">
        <div class="scheduler-resource-head">
          <h3><span class="icon">▦</span> Наборы предметов</h3>
          <button type="button" class="mini-action" data-array-add="ItemSets">Добавить набор</button>
        </div>
        ${setRows || '<div class="muted-line">Наборов пока нет.</div>'}
      </section>
    </div>
  </div>`;
}

schedulerModeMeta.Airdrop = {
  label: 'Cargo drop',
  help: 'Запускает настоящий SCUM cargo drop командой ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}.'
};
schedulerModeMeta.WorldEvent = {
  label: 'World event',
  help: 'Запускает выбранный класс события командой ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}.'
};
schedulerModeMeta.SpawnItems = {
  label: 'Лут в точку',
  help: 'Заспавнить выбранный набор предметов прямо в выбранной точке карты через SpawnItem.'
};
schedulerModeMeta.Command = {
  label: 'Команда',
  help: 'Выполнить одну безопасную серверную команду по времени или интервалу.'
};

function schedulerReadableSummary(job) {
  const mode = schedulerMode(job || {});
  const times = String(schedulerValue(job, 'ScheduleTimes', '') || '').trim();
  const interval = Number(schedulerValue(job, 'IntervalMinutes', 0) || 0);
  const modeLabel = schedulerModeMeta[mode] ? schedulerModeMeta[mode].label : mode;
  const pointCount = ['Airdrop', 'WorldEvent'].includes(mode) ? Math.max(Number(schedulerValue(job, 'PointCount', 1) || 1), 1) : 1;
  const suffix = pointCount > 1 ? ` · точек ${pointCount}` : '';
  if (times) return `${modeLabel}${suffix} · в ${times}`;
  return `${modeLabel}${suffix} · каждые ${interval || 0} мин.`;
}

function renderSchedulerResourceRows(arrayKey, rows, labels, emptyText) {
  if (!rows.length) return `<div class="muted-line">${escapeHtml(emptyText)}</div>`;
  return rows.map((row, index) => `<div class="scheduler-resource-row scheduler-resource-row-simple">
    <div class="scheduler-resource-fields ${arrayKey === 'ItemSets' ? 'itemset' : ''}">
      ${labels.map(([field, label, kind]) => renderArrayInput(arrayKey, index, field, label, kind, null, row || {})).join('')}
    </div>
    <button type="button" class="mini-action danger" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}">Удалить</button>
  </div>`).join('');
}

renderSchedulerJobsEditor = function renderSchedulerJobsEditor(config) {
  const jobs = Array.isArray(config && config.Jobs) ? config.Jobs : [];
  const points = Array.isArray(config && config.Points) ? config.Points : [];
  const itemSets = Array.isArray(config && config.ItemSets) ? config.ItemSets : [];
  if (!Number.isInteger(state.schedulerJobIndex) || !jobs[state.schedulerJobIndex]) state.schedulerJobIndex = jobs.length ? 0 : null;
  const selectedIndex = state.schedulerJobIndex;
  const selectedJob = selectedIndex !== null ? jobs[selectedIndex] : null;
  const selectedMode = schedulerMode(selectedJob || {});
  const jobCards = jobs.map((job, index) => {
    const enabled = schedulerValue(job, 'Enabled', true) !== false;
    const mode = schedulerMode(job);
    return `<article class="scheduler-job-card scheduler-job-card-compact ${selectedIndex === index ? 'active' : ''}" data-scheduler-job-open="${index}">
      <div class="scheduler-job-main">
        <b>${escapeHtml(schedulerValue(job, 'Name', `Задание #${index + 1}`))}</b>
        <span>${escapeHtml(schedulerReadableSummary(job))}</span>
      </div>
      <div class="scheduler-job-tags">
        <span>${escapeHtml(schedulerModeMeta[mode].label)}</span>
      </div>
      <button type="button" class="mini-action danger" data-array-remove="Jobs" data-array-index="${index}">Удалить</button>
    </article>`;
  }).join('');

  const actionFields = selectedJob ? (() => {
    if (selectedMode === 'SpawnItems') {
      return `${renderSchedulerArrayControl('Jobs', selectedIndex, 'PointGroup', 'Группа точек', 'Например: airdrop или stash. Берётся случайная точка из списка ниже.', 'text')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'ItemSet', 'Набор лута', 'Имя набора из блока ниже. Предметы появятся именно в координатах выбранной точки.', 'text')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'MaxItemsPerRun', 'Лимит предметов', 'Защита от случайного массового спавна.', 'number')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Announcement', 'Сообщение игрокам', 'Можно использовать {Point} и {ItemSet}.', 'text')}`;
    }
    if (selectedMode === 'Airdrop') {
      return `${renderSchedulerArrayControl('Jobs', selectedIndex, 'PointGroup', 'Группа точек', 'Из этой группы будет выбрана точка для cargo drop.', 'text')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'PointCount', 'Сколько точек взять', '1 = прежний режим: одна случайная точка. Больше 1 = за один запуск взять несколько разных точек из группы и вызвать cargo drop на каждой.', 'number')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'CommandTemplate', 'Команда', 'Точный формат: ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}.', 'textarea')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Announcement', 'Сообщение игрокам', 'Можно использовать {Point}.', 'text')}`;
    }
    if (selectedMode === 'WorldEvent') {
      return `${renderSchedulerArrayControl('Jobs', selectedIndex, 'PointGroup', 'Группа точек', 'Из этой группы будет выбрана точка для world event.', 'text')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'PointCount', 'Сколько точек взять', 'Можно запустить событие в нескольких уникальных точках группы за один запуск.', 'number')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'WorldEventClass', 'Класс события', 'Класс из dump-подтверждённых world events.', schedulerWorldEventClassSelect)}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'CommandTemplate', 'Команда', 'Обычно: ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}.', 'textarea')}
        ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Announcement', 'Сообщение игрокам', 'Можно использовать {Point} и {WorldEventClass}.', 'text')}`;
    }
    return `${renderSchedulerArrayControl('Jobs', selectedIndex, 'CommandTemplate', 'Команда', 'Например: Announce Рестарт через 10 минут.', 'textarea')}
      ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Announcement', 'Сообщение после выполнения', 'Необязательно. Оставь пустым, если команда сама пишет в чат.', 'text')}`;
  })() : '';

  const workspace = selectedJob ? `<section class="scheduler-workspace scheduler-workspace-simple">
    <div class="scheduler-workspace-head">
      <div>
        <span class="wargm-modal-kicker">Задание по расписанию</span>
        <h3>${escapeHtml(schedulerValue(selectedJob, 'Name', `Задание #${selectedIndex + 1}`))}</h3>
        <p>${escapeHtml(schedulerModeMeta[selectedMode].help)}</p>
      </div>
      <span class="wargm-mode-pill">${escapeHtml(schedulerModeMeta[selectedMode].label)}</span>
    </div>
    <div class="scheduler-simple-grid">
      <section class="setting-group card">
        <h3><span class="icon">1</span> Что это за задание</h3>
        <div class="setting-list">
          ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Enabled', 'Включено', 'Выключи, если хочешь сохранить, но временно не запускать.', 'checkbox')}
          ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Name', 'Название', 'Будет видно слева в списке.', 'text')}
          ${renderSchedulerArrayControl('Jobs', selectedIndex, 'Mode', 'Что сделать', 'Команда, cargo drop, world event или набор предметов.', 'select:Airdrop|WorldEvent|SpawnItems|Command')}
          ${renderSchedulerArrayControl('Jobs', selectedIndex, 'RunOnStartup', 'Запустить при старте', 'Один раз после запуска сервера.', 'checkbox')}
        </div>
      </section>
      <section class="setting-group card">
        <h3><span class="icon">2</span> Когда запускать</h3>
        <div class="setting-list">
          ${renderSchedulerArrayControl('Jobs', selectedIndex, 'ScheduleTimes', 'Точное время', 'Например: 12:00, 18:30. Если указано, интервал не нужен.', 'text')}
          ${renderSchedulerArrayControl('Jobs', selectedIndex, 'IntervalMinutes', 'Интервал, минут', 'Работает, если поле точного времени пустое.', 'number')}
        </div>
      </section>
      <section class="setting-group card scheduler-action-card">
        <h3><span class="icon">3</span> Что выполнить</h3>
        <div class="setting-list">${actionFields}</div>
      </section>
    </div>
  </section>` : `<section class="scheduler-workspace empty">
    <h3>Заданий пока нет</h3>
    <p>Нажми “Добавить задание”, выбери тип и сохрани планировщик.</p>
  </section>`;

  const pointRows = renderSchedulerResourceRows('Points', points, [
    ['Name', 'Название', 'text'],
    ['Group', 'Группа', 'text'],
    ['X', 'X', 'number'],
    ['Y', 'Y', 'number'],
    ['Z', 'Z', 'number'],
    ['Radius', 'Радиус', 'number']
  ], 'Точек пока нет. Добавь точки для cargo drop или спавна наборов.');

  const setRows = renderSchedulerResourceRows('ItemSets', itemSets, [
    ['Name', 'Название', 'text'],
    ['Weight', 'Вес выбора', 'number'],
    ['ItemsText', 'Предметы: ItemId|Кол-во;...', 'textarea']
  ], 'Наборов пока нет. Добавь набор для режима “Лут в точку”.');

  return `<div class="field wide scheduler-editor scheduler-editor-simple">
    <div class="settings-header scheduler-header">
      <div class="header-info">
        <h2>Планировщик заданий</h2>
        <span class="sub-text">Простая схема: задание слева, настройка справа, точки и наборы ниже.</span>
      </div>
      <div class="scheduler-header-actions">
        <button type="button" class="btn" data-array-add="Jobs">Добавить задание</button>
        <button type="button" class="btn primary" data-scheduler-save>Сохранить планировщик</button>
      </div>
    </div>
    <div class="scheduler-top-grid scheduler-top-grid-simple">
      <div class="setting-group card">
        <h3><span class="icon">✓</span> Общие настройки</h3>
        <div class="setting-list">
          ${renderSchedulerTopControl('Enabled', 'Планировщик включен', 'Главный выключатель всех заданий.', 'checkbox')}
          ${renderSchedulerTopControl('Name', 'Название модуля', 'Отображается только в панели.', 'text')}
          ${renderSchedulerTopControl('PollIntervalMs', 'Проверка расписания, мс', '15000 = проверка раз в 15 секунд. Ниже лучше не ставить без нужды.', 'number')}
          ${renderSchedulerTopControl('MaxRunsPerTick', 'Задач за один тик', 'Оставь 1, чтобы сервер не получал залп команд.', 'number')}
        </div>
      </div>
      <div class="setting-group card scheduler-help">
        <h3><span class="icon">i</span> Подсказка</h3>
        <p>Если нужно запускать строго по времени, заполни <b>Точное время</b>. Если нужно повторять каждые N минут, оставь точное время пустым и укажи интервал.</p>
      </div>
    </div>
    <div class="scheduler-shell">
      <aside class="scheduler-job-list">
        ${jobCards || '<div class="muted-line">Заданий пока нет. Нажми “Добавить задание”.</div>'}
      </aside>
      ${workspace}
    </div>
    <div class="scheduler-resources-grid scheduler-resources-grid-simple">
      <section class="setting-group card">
        <div class="scheduler-resource-head">
          <h3><span class="icon">•</span> Точки карты</h3>
          <button type="button" class="mini-action" data-array-add="Points">Добавить точку</button>
        </div>
        ${pointRows}
      </section>
      <section class="setting-group card">
        <div class="scheduler-resource-head">
          <h3><span class="icon">▦</span> Наборы предметов</h3>
          <button type="button" class="mini-action" data-array-add="ItemSets">Добавить набор</button>
        </div>
        ${setRows}
      </section>
    </div>
  </div>`;
};

function schedulerArrayKey(config, wanted, fallback) {
  const actual = Object.keys(config || {}).find(key => key.toLowerCase() === wanted.toLowerCase());
  return actual || fallback || wanted;
}

function schedulerConfigNumber(config, key, fallback) {
  const value = Number(schedulerValue(config || {}, key, fallback));
  return Number.isFinite(value) ? value : fallback;
}

function renderSchedulerConfigControl(key, label, hint, kind = 'text') {
  const value = schedulerValue(state.moduleDraft || {}, key, kind === 'checkbox' ? false : '');
  if (kind === 'checkbox') {
    return `<div class="setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
      <div class="setting-control">${renderToggleInput(`data-config-key="${escapeAttr(key)}"`, !!value)}</div>
    </div>`;
  }
  const inputKind = kind === 'number' ? 'number' : 'text';
  return `<div class="setting-row">
    <div class="setting-label"><label>${escapeHtml(label)}</label><span>${escapeHtml(hint || '')}</span></div>
    <div class="setting-control"><input data-config-key="${escapeAttr(key)}" type="${inputKind}" value="${escapeAttr(value ?? '')}" /></div>
  </div>`;
}

function schedulerJobTitle(job, index) {
  return schedulerValue(job || {}, 'Name', `Задание #${index + 1}`) || `Задание #${index + 1}`;
}

function schedulerJobSearchText(job, index) {
  return [
    schedulerJobTitle(job, index),
    schedulerReadableSummary(job || {}),
    schedulerValue(job || {}, 'PointGroup', ''),
    schedulerValue(job || {}, 'ItemSet', ''),
    schedulerValue(job || {}, 'WorldEventClass', ''),
    schedulerValue(job || {}, 'CommandTemplate', ''),
    schedulerValue(job || {}, 'Announcement', '')
  ].join(' ').toLowerCase();
}

function schedulerActionSummary(job) {
  const mode = schedulerMode(job || {});
  if (mode === 'Airdrop') {
    return `cargo: ${schedulerValue(job, 'PointGroup', '') || 'точки не указаны'}`;
  }
  if (mode === 'WorldEvent') {
    return `${schedulerValue(job, 'WorldEventClass', '') || 'класс не указан'} · ${schedulerValue(job, 'PointGroup', '') || 'точки не указаны'}`;
  }
  if (mode === 'SpawnItems') {
    return `${schedulerValue(job, 'ItemSet', '') || 'набор не указан'} · лут в: ${schedulerValue(job, 'PointGroup', '') || 'точки не указаны'}`;
  }
  return schedulerValue(job, 'CommandTemplate', '') || 'команда не указана';
}

function renderSchedulerJobActionFields(arrayKey, index, job) {
  const mode = schedulerMode(job || {});
  if (mode === 'Airdrop') {
    return `${renderSchedulerArrayControl(arrayKey, index, 'PointGroup', 'Группа точек', 'Из этой группы будет выбрана точка для cargo drop.', 'text')}
      ${renderSchedulerArrayControl(arrayKey, index, 'PointCount', 'Сколько точек взять', '1 = прежний режим: одна случайная точка. Больше 1 = за один запуск взять несколько разных точек из группы и вызвать cargo drop на каждой.', 'number')}
      ${renderSchedulerArrayControl(arrayKey, index, 'CommandTemplate', 'Команда', 'Точный формат: ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}.', 'textarea')}
      ${renderSchedulerArrayControl(arrayKey, index, 'Announcement', 'Сообщение игрокам', 'Можно использовать {Point}.', 'text')}`;
  }
  if (mode === 'WorldEvent') {
    return `${renderSchedulerArrayControl(arrayKey, index, 'PointGroup', 'Группа точек', 'Из этой группы будет выбрана точка для world event.', 'text')}
      ${renderSchedulerArrayControl(arrayKey, index, 'PointCount', 'Сколько точек взять', 'Можно запустить событие в нескольких уникальных точках группы за один запуск.', 'number')}
      ${renderSchedulerArrayControl(arrayKey, index, 'WorldEventClass', 'Класс события', 'Класс из dump-подтверждённых world events.', schedulerWorldEventClassSelect)}
      ${renderSchedulerArrayControl(arrayKey, index, 'CommandTemplate', 'Команда', 'Обычно: ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}.', 'textarea')}
      ${renderSchedulerArrayControl(arrayKey, index, 'Announcement', 'Сообщение игрокам', 'Можно использовать {Point} и {WorldEventClass}.', 'text')}`;
  }
  if (mode === 'SpawnItems') {
    return `${renderSchedulerArrayControl(arrayKey, index, 'PointGroup', 'Группа точек', 'Например: airdrop. Предметы появятся прямо в координатах выбранной точки.', 'text')}
      ${renderSchedulerArrayControl(arrayKey, index, 'ItemSet', 'Набор лута', 'Имя набора из вкладки “Точки и наборы”.', 'text')}
      ${renderSchedulerArrayControl(arrayKey, index, 'MaxItemsPerRun', 'Лимит предметов', 'Защита от случайного массового спавна.', 'number')}
      ${renderSchedulerArrayControl(arrayKey, index, 'Announcement', 'Сообщение игрокам', 'Можно использовать {Point} и {ItemSet}.', 'text')}`;
  }
  return `${renderSchedulerArrayControl(arrayKey, index, 'CommandTemplate', 'Команда', 'Например: Announce Рестарт через 10 минут. Символ # добавится автоматически.', 'textarea')}
    ${renderSchedulerArrayControl(arrayKey, index, 'Announcement', 'Сообщение после выполнения', 'Необязательно. Оставь пустым, если команда сама пишет в чат.', 'text')}`;
}

function renderSchedulerJobModal(arrayKey, index, job) {
  const mode = schedulerMode(job || {});
  const meta = schedulerModeMeta[mode] || schedulerModeMeta.Command;
  const enabled = schedulerValue(job || {}, 'Enabled', true) !== false;
  return `<div class="scheduler-job-modal wargm-rule-modal" data-scheduler-job-overlay="true" role="dialog" aria-modal="true">
    <div class="scheduler-job-modal-card wargm-rule-modal-card">
      <div class="wargm-rule-modal-head">
        <div>
          <span class="wargm-modal-kicker">Настройка задания</span>
          <h3>${escapeHtml(schedulerJobTitle(job || {}, index))}</h3>
          <p>${escapeHtml(schedulerReadableSummary(job || {}))}</p>
        </div>
        <div class="wargm-rule-modal-actions">
          <span class="wargm-mode-pill">${escapeHtml(meta.label)}</span>
          <button type="button" class="mini-action" data-scheduler-job-run="${index}">Запустить сейчас</button>
          <button type="button" class="mini-action primary" data-scheduler-job-save="true">Сохранить задание</button>
          <button type="button" class="mini-action" data-scheduler-job-close="true">Закрыть</button>
        </div>
      </div>
      <div class="scheduler-job-modal-body">
        <section class="wargm-rule-section">
          <div class="wargm-rule-section-title">
            <div><h4>1. Основное</h4><p>Название, тип и включение конкретного задания.</p></div>
            <label class="mini-check toggle-line"><span>${enabled ? 'Включено' : 'Выключено'}</span>${renderToggleInput(`data-array-key="${escapeAttr(arrayKey)}" data-array-index="${index}" data-array-field="Enabled"`, enabled)}</label>
          </div>
          <div class="array-grid scheduler-job-modal-grid">
            ${renderSchedulerArrayControl(arrayKey, index, 'Name', 'Название', 'Будет видно в списке задач и логах.', 'text')}
            ${renderSchedulerArrayControl(arrayKey, index, 'Mode', 'Что сделать', 'Команда, cargo drop, world event или набор предметов.', 'select:Airdrop|WorldEvent|SpawnItems|Command')}
            ${renderSchedulerArrayControl(arrayKey, index, 'RunOnStartup', 'Запуск при старте', 'Один раз после запуска сервера.', 'checkbox')}
          </div>
        </section>
        <section class="wargm-rule-section">
          <div class="wargm-rule-section-title">
            <div><h4>2. Когда запускать</h4><p>Точное время имеет приоритет. Если оно пустое, работает интервал.</p></div>
          </div>
          <div class="array-grid scheduler-job-modal-grid">
            ${renderSchedulerArrayControl(arrayKey, index, 'ScheduleTimes', 'Точное время', 'Например: 12:00, 18:30.', 'text')}
            ${renderSchedulerArrayControl(arrayKey, index, 'IntervalMinutes', 'Интервал, минут', 'Работает, если точное время не указано.', 'number')}
          </div>
        </section>
        <section class="wargm-rule-section">
          <div class="wargm-rule-section-title">
            <div><h4>3. Действие</h4><p>${escapeHtml(meta.help)}</p></div>
          </div>
          <div class="array-grid scheduler-job-modal-grid action">${renderSchedulerJobActionFields(arrayKey, index, job || {})}</div>
        </section>
      </div>
    </div>
  </div>`;
}

function renderSchedulerJobsPanel(arrayKey, rows) {
  const jobs = Array.isArray(rows) ? rows : [];
  if (!Number.isInteger(state.schedulerJobEditIndex) || !jobs[state.schedulerJobEditIndex]) finishSchedulerJobEditor();
  const search = String(state.schedulerJobSearch || '').trim();
  const q = search.toLowerCase();
  let visible = 0;
  const cards = jobs.map((job, index) => {
    const enabled = schedulerValue(job || {}, 'Enabled', true) !== false;
    const mode = schedulerMode(job || {});
    const meta = schedulerModeMeta[mode] || schedulerModeMeta.Command;
    const searchText = schedulerJobSearchText(job || {}, index);
    const matches = !q || searchText.includes(q);
    if (matches) visible += 1;
    return `<article class="scheduler-job-card-v2 wargm-rule-card ${state.schedulerJobEditIndex === index ? 'active' : ''}" data-scheduler-job-card="true" data-scheduler-job-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
      <div class="wargm-rule-card-main">
        <div>
          <b>${escapeHtml(schedulerJobTitle(job || {}, index))}</b>
          <span class="wargm-rule-card-status ${enabled ? 'on' : 'off'}">${enabled ? 'вкл' : 'выкл'}</span>
        </div>
        <span>${escapeHtml(schedulerReadableSummary(job || {}))}</span>
      </div>
      <div class="wargm-rule-card-meta">
        <span>${escapeHtml(meta.label)}</span>
        <span>${escapeHtml(schedulerActionSummary(job || {}))}</span>
      </div>
      <div class="wargm-rule-card-actions">
        <button type="button" class="mini-action primary" data-scheduler-job-open="${index}">Настроить</button>
        <button type="button" class="mini-action" data-scheduler-job-run="${index}">Запустить</button>
        <button type="button" class="mini-action danger" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}">Удалить</button>
      </div>
    </article>`;
  }).join('');
  return `<div class="field wide array-editor scheduler-jobs-panel">
    <div class="array-title">
      <div>
        <label>Задачи планировщика</label>
        <p>Список занимает всю рабочую область. Настройка открывается отдельным окном, как в Wargm.</p>
      </div>
      <button type="button" class="mini-action" data-array-add="${escapeAttr(arrayKey)}">Добавить задание</button>
    </div>
    <div class="wargm-rules-toolbar scheduler-jobs-toolbar">
      <input class="wargm-rule-search" data-scheduler-job-search placeholder="Поиск задачи: название, команда, точка, набор" value="${escapeAttr(search)}" />
      <span class="wargm-rule-search-count" data-scheduler-job-search-count>${visible} / ${jobs.length}</span>
    </div>
    <div class="wargm-rule-list scheduler-job-list-v2">
      ${cards}
      <div class="muted-line scheduler-job-search-empty" data-scheduler-job-search-empty ${visible ? 'hidden' : ''}>Задачи не найдены.</div>
    </div>
  </div>`;
}

function clearSchedulerJobPortal() {
  document.querySelectorAll('[data-scheduler-job-portal="true"]').forEach(node => node.remove());
}

function discardNewSchedulerJobIfNeeded() {
  if (!state.schedulerJobEditIsNew || !state.moduleDraft) return false;
  const index = state.schedulerJobEditIndex;
  const jobsKey = schedulerArrayKey(state.moduleDraft || {}, 'Jobs', 'Jobs');
  const jobs = Array.isArray(state.moduleDraft[jobsKey]) ? state.moduleDraft[jobsKey] : null;
  if (!Number.isInteger(index) || !jobs || !jobs[index]) return false;
  jobs.splice(index, 1);
  setModuleDraftText();
  return true;
}

function closeSchedulerJobEditor() {
  discardNewSchedulerJobIfNeeded();
  state.schedulerJobEditIndex = null;
  state.schedulerJobEditIsNew = false;
}

function finishSchedulerJobEditor() {
  state.schedulerJobEditIndex = null;
  state.schedulerJobEditIsNew = false;
}

function openSchedulerJobEditor(index, isNew = false) {
  state.schedulerJobEditIndex = Number(index);
  state.schedulerJobEditIsNew = isNew === true;
}

function renderSchedulerJobPortal() {
  clearSchedulerJobPortal();
  if (String(state.selectedModule || '').toLowerCase() !== 'scheduled-events') return;
  const config = state.moduleDraft || {};
  const jobsKey = schedulerArrayKey(config, 'Jobs', 'Jobs');
  const jobs = Array.isArray(config[jobsKey]) ? config[jobsKey] : [];
  if (!Number.isInteger(state.schedulerJobEditIndex) || !jobs[state.schedulerJobEditIndex]) return;
  const holder = document.createElement('div');
  holder.innerHTML = renderSchedulerJobModal(jobsKey, state.schedulerJobEditIndex, jobs[state.schedulerJobEditIndex]);
  const modal = holder.firstElementChild;
  if (!modal) return;
  modal.dataset.schedulerJobPortal = 'true';
  document.body.appendChild(modal);
}

function renderSchedulerResourceRows(arrayKey, rows, labels, emptyText) {
  const list = Array.isArray(rows) ? rows : [];
  if (!list.length) return `<div class="muted-line">${escapeHtml(emptyText)}</div>`;
  return list.map((row, index) => `<div class="scheduler-resource-row scheduler-resource-row-simple">
    <div class="scheduler-resource-fields ${arrayKey.toLowerCase() === 'itemsets' ? 'itemset' : ''}">
      ${labels.map(([field, label, kind]) => renderArrayInput(arrayKey, index, field, label, kind, null, row || {})).join('')}
    </div>
    <button type="button" class="mini-action danger" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}">Удалить</button>
  </div>`).join('');
}

function renderSchedulerResourcesPanel(config) {
  const pointsKey = schedulerArrayKey(config, 'Points', 'Points');
  const itemSetsKey = schedulerArrayKey(config, 'ItemSets', 'ItemSets');
  const points = Array.isArray(config && config[pointsKey]) ? config[pointsKey] : [];
  const itemSets = Array.isArray(config && config[itemSetsKey]) ? config[itemSetsKey] : [];
  const pointRows = renderSchedulerResourceRows(pointsKey, points, [
    ['Name', 'Название', 'text'],
    ['Group', 'Группа', 'text'],
    ['X', 'X', 'number'],
    ['Y', 'Y', 'number'],
    ['Z', 'Z', 'number'],
    ['Radius', 'Радиус', 'number']
  ], 'Точек пока нет. Добавь точку для cargo drop или спавна набора.');
  const setRows = renderSchedulerResourceRows(itemSetsKey, itemSets, [
    ['Name', 'Название', 'text'],
    ['Weight', 'Вес выбора', 'number'],
    ['ItemsText', 'Предметы: ItemId|Кол-во;...', 'textarea']
  ], 'Наборов пока нет. Добавь набор для режима “Лут в точку”.');
  return `<div class="scheduler-resources-page">
    <section class="setting-group card">
      <div class="scheduler-resource-head">
        <div><h3><span class="icon">1</span> Точки карты</h3><p>Группа связывает задание с точками. Радиус даёт случайный разброс в этой зоне.</p></div>
        <button type="button" class="mini-action" data-array-add="${escapeAttr(pointsKey)}">Добавить точку</button>
      </div>
      ${pointRows}
    </section>
    <section class="setting-group card">
      <div class="scheduler-resource-head">
        <div><h3><span class="icon">2</span> Наборы предметов</h3><p>Формат строки: ItemId|Количество;ItemId2|Количество.</p></div>
        <button type="button" class="mini-action" data-array-add="${escapeAttr(itemSetsKey)}">Добавить набор</button>
      </div>
      ${setRows}
    </section>
  </div>`;
}

function renderSchedulerModuleFields(config) {
  const jobsKey = schedulerArrayKey(config, 'Jobs', 'Jobs');
  const jobs = Array.isArray(config && config[jobsKey]) ? config[jobsKey] : [];
  const pointsKey = schedulerArrayKey(config, 'Points', 'Points');
  const itemSetsKey = schedulerArrayKey(config, 'ItemSets', 'ItemSets');
  const points = Array.isArray(config && config[pointsKey]) ? config[pointsKey] : [];
  const itemSets = Array.isArray(config && config[itemSetsKey]) ? config[itemSetsKey] : [];
  const active = ['settings', 'jobs', 'resources'].includes(state.schedulerConfigTab) ? state.schedulerConfigTab : 'settings';
  const poll = schedulerConfigNumber(config, 'PollIntervalMs', 15000);
  const runs = schedulerConfigNumber(config, 'MaxRunsPerTick', 1);
  const settings = `<section class="setting-group card module-basic-settings scheduler-main-settings">
    <h3><span class="icon">⚙</span> Основные настройки</h3>
    <div class="setting-list">
      ${renderSchedulerConfigControl('Enabled', 'Планировщик включен', 'Главный выключатель всех задач.', 'checkbox')}
      ${renderSchedulerConfigControl('Name', 'Название модуля', 'Отображается только в панели.', 'text')}
      ${renderSchedulerConfigControl('Description', 'Описание', 'Короткая подсказка для админов.', 'text')}
      ${renderSchedulerConfigControl('PollIntervalMs', 'Проверка расписания, мс', `Сейчас ${poll}. Рекомендовано 15000, минимум на сервере 5000.`, 'number')}
      ${renderSchedulerConfigControl('MaxRunsPerTick', 'Задач за одну проверку', `Сейчас ${runs}. Для стабильности держи 1.`, 'number')}
    </div>
  </section>
  <section class="setting-group card scheduler-help">
    <h3><span class="icon">i</span> Как пользоваться</h3>
    <p>Для запуска по времени заполни <b>Точное время</b>, например 12:00 или 12:00, 18:30. Для повтора каждые N минут оставь точное время пустым и заполни интервал. Кнопка “Запустить” сохраняет конфиг и сразу выполняет выбранную задачу для проверки.</p>
  </section>`;
  return `<div class="module-settings-view scheduler-module-view">
    <div class="wargm-module-tabs scheduler-module-tabs" role="tablist" aria-label="Планировщик задач">
      <button type="button" class="tab wargm-module-tab ${active === 'settings' ? 'active' : ''}" data-scheduler-config-tab="settings" role="tab" aria-selected="${active === 'settings'}">Основные настройки</button>
      <button type="button" class="tab wargm-module-tab ${active === 'jobs' ? 'active' : ''}" data-scheduler-config-tab="jobs" role="tab" aria-selected="${active === 'jobs'}">Задачи <span>${jobs.length}</span></button>
      <button type="button" class="tab wargm-module-tab ${active === 'resources' ? 'active' : ''}" data-scheduler-config-tab="resources" role="tab" aria-selected="${active === 'resources'}">Точки и наборы <span>${points.length + itemSets.length}</span></button>
    </div>
    <div class="wargm-module-tab-panel" data-scheduler-config-panel="settings" ${active === 'settings' ? '' : 'hidden'}>${settings}</div>
    <div class="wargm-module-tab-panel scheduler-jobs-tab" data-scheduler-config-panel="jobs" ${active === 'jobs' ? '' : 'hidden'}>${renderSchedulerJobsPanel(jobsKey, jobs)}</div>
    <div class="wargm-module-tab-panel scheduler-resources-tab" data-scheduler-config-panel="resources" ${active === 'resources' ? '' : 'hidden'}>${renderSchedulerResourcesPanel(config || {})}</div>
  </div>`;
}

function updateSchedulerJobSearchFilter() {
  const input = els.moduleFields && els.moduleFields.querySelector('[data-scheduler-job-search]');
  if (!input) return;
  const query = String(input.value || '').trim();
  const q = query.toLowerCase();
  state.schedulerJobSearch = query;
  let visible = 0;
  let total = 0;
  els.moduleFields.querySelectorAll('[data-scheduler-job-card]').forEach(card => {
    total += 1;
    const match = !q || String(card.dataset.schedulerJobSearchText || '').includes(q);
    card.hidden = !match;
    if (match) visible += 1;
  });
  const count = els.moduleFields.querySelector('[data-scheduler-job-search-count]');
  if (count) count.textContent = `${visible} / ${total}`;
  const empty = els.moduleFields.querySelector('[data-scheduler-job-search-empty]');
  if (empty) empty.hidden = visible > 0;
}

function normalizeSchedulerConfigForSave(config) {
  const clone = JSON.parse(JSON.stringify(config || {}));
  const pollKey = Object.keys(clone).find(key => key.toLowerCase() === 'pollintervalms') || 'PollIntervalMs';
  const runsKey = Object.keys(clone).find(key => key.toLowerCase() === 'maxrunspertick') || 'MaxRunsPerTick';
  clone[pollKey] = Math.min(Math.max(Number(clone[pollKey] || 15000), 5000), 600000);
  clone[runsKey] = Math.min(Math.max(Number(clone[runsKey] || 1), 1), 10);
  const normalizeCargoDropTemplate = value => {
    const raw = String(value || '');
    const key = raw.trim().replace(/^#\s*/, '').replace(/\s+/g, ' ').toLowerCase();
    if (key === 'scheduleworldevent cargodrop {x} {y} {z}' || key === 'scheduleworldevent bp_cargodropevent {x} {y} {z}') {
      return 'ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}';
    }
    return value;
  };
  const jobsKey = schedulerArrayKey(clone, 'Jobs', 'Jobs');
  if (Array.isArray(clone[jobsKey])) {
    clone[jobsKey].forEach(job => {
      if (!job || typeof job !== 'object') return;
      if (!schedulerValue(job, 'Name', '')) job.Name = 'Задание';
      const normalizedMode = schedulerMode(job);
      job.Mode = normalizedMode;
      const intervalKey = Object.keys(job).find(key => key.toLowerCase() === 'intervalminutes') || 'IntervalMinutes';
      job[intervalKey] = Math.max(Number(job[intervalKey] || 60), 1);
      const pointCountKey = Object.keys(job).find(key => key.toLowerCase() === 'pointcount') || 'PointCount';
      job[pointCountKey] = Math.min(Math.max(Number(job[pointCountKey] || 1), 1), 10);
      const maxItemsKey = Object.keys(job).find(key => key.toLowerCase() === 'maxitemsperrun') || 'MaxItemsPerRun';
      job[maxItemsKey] = Math.min(Math.max(Number(job[maxItemsKey] || 30), 1), 300);
      const eventClassKey = Object.keys(job).find(key => key.toLowerCase() === 'worldeventclass' || key.toLowerCase() === 'eventclass') || 'WorldEventClass';
      if (normalizedMode === 'WorldEvent') {
        const eventClass = String(job[eventClassKey] || '').trim();
        job[eventClassKey] = /^[A-Za-z0-9_.-]+$/.test(eventClass) ? eventClass : 'BP_CargoDropEvent';
      }
      const commandKey = Object.keys(job).find(key => key.toLowerCase() === 'commandtemplate') || 'CommandTemplate';
      job[commandKey] = normalizeCargoDropTemplate(job[commandKey]);
      if (normalizedMode === 'WorldEvent' && !String(job[commandKey] || '').trim()) {
        job[commandKey] = 'ScheduleWorldEvent {WorldEventClass} X={X} Y={Y} Z={Z}';
      }
      if (normalizedMode === 'Airdrop' && !String(job[commandKey] || '').trim()) {
        job[commandKey] = 'ScheduleWorldEvent BP_CargoDropEvent X={X} Y={Y} Z={Z}';
      }
    });
  }
  return clone;
}

function renderArrayEditor(key, value, fields) {
  if (String(key).toLowerCase() === 'rules' && isExternalShopModule()) return renderWargmRulesEditor(key, value, fields);
  const rows = Array.isArray(value) ? value : [];
  const body = rows.map((row, index) => `<div class="array-row">
    <div class="array-row-head">
      <b>${escapeHtml(configLabel(key))} #${index + 1}</b>
      <button type="button" class="mini-action danger" data-array-remove="${escapeAttr(key)}" data-array-index="${index}">Удалить</button>
    </div>
    <div class="array-grid">
      ${fields.map(([field, label, kind, listId]) => renderArrayInput(key, index, field, label, kind, listId, row || {})).join('')}
    </div>
  </div>`).join('');
  return `<div class="field wide array-editor">
    <div class="array-title">
      <label>${escapeHtml(configLabel(key))}</label>
      <button type="button" class="mini-action" data-array-add="${escapeAttr(key)}">Добавить</button>
    </div>
    ${body || '<div class="muted-line">Пока пусто.</div>'}
  </div>`;
}

function renderPrimitiveArray(key, value) {
  const mode = key === 'WarningMinutesBeforeExpiry' ? 'number-list' : 'line-list';
  const text = Array.isArray(value) ? value.join(mode === 'number-list' ? ', ' : '\n') : '';
  return `<div class="field wide"><label>${escapeHtml(configLabel(key))}</label><textarea data-config-key="${escapeAttr(key)}" data-config-mode="${mode}">${escapeHtml(text)}</textarea></div>`;
}

function renderObjectEditor(key, value) {
  const object = value && typeof value === 'object' ? value : {};
  if (Object.values(object).every(v => typeof v === 'number')) {
    const inputs = Object.entries(object).map(([field, fieldValue]) => `<label><span>${escapeHtml(configLabel(field))}</span><input data-object-key="${escapeAttr(key)}" data-object-field="${escapeAttr(field)}" type="number" value="${escapeAttr(fieldValue)}" /></label>`).join('');
    return `<div class="field wide object-editor"><label>${escapeHtml(configLabel(key))}</label><div class="array-grid">${inputs}</div></div>`;
  }
  return `<div class="field wide"><label>${escapeHtml(configLabel(key))}</label><textarea data-config-key="${escapeAttr(key)}" spellcheck="false">${escapeHtml(JSON.stringify(object, null, 2))}</textarea></div>`;
}

function renderConfigSettingRow(key, value) {
  const safeKey = escapeAttr(key);
  const label = configLabel(key);
  const hint = key in configWordLabels ? key : '';
  if (typeof value === 'boolean') {
    return `<div class="setting-row module-setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label>${hint ? `<span>${escapeHtml(hint)}</span>` : ''}</div>
      <div class="setting-control">${renderToggleInput(`data-config-key="${safeKey}"`, value)}</div>
    </div>`;
  }
  if (typeof value === 'number') {
    return `<div class="setting-row module-setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label>${hint ? `<span>${escapeHtml(hint)}</span>` : ''}</div>
      <div class="setting-control"><input data-config-key="${safeKey}" type="number" value="${escapeAttr(value)}" /></div>
    </div>`;
  }
  if (typeof value === 'string' && key === 'TransportMode') {
    const options = ['Webhook', 'BotChannel'].map(opt => `<option value="${escapeAttr(opt)}" ${String(value).toLowerCase() === opt.toLowerCase() ? 'selected' : ''}>${escapeHtml(opt === 'BotChannel' ? 'Bot Channel' : 'Webhook')}</option>`).join('');
    return `<div class="setting-row module-setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label><span>Discord delivery mode</span></div>
      <div class="setting-control"><select data-config-key="${safeKey}">${options}</select></div>
    </div>`;
  }
  if (typeof value === 'string') {
    return `<div class="setting-row module-setting-row">
      <div class="setting-label"><label>${escapeHtml(label)}</label>${hint ? `<span>${escapeHtml(hint)}</span>` : ''}</div>
      <div class="setting-control"><input data-config-key="${safeKey}" value="${escapeAttr(value)}" /></div>
    </div>`;
  }
  return `<div class="field wide"><label>${escapeHtml(label)}</label><textarea data-config-key="${safeKey}" spellcheck="false">${escapeHtml(JSON.stringify(value, null, 2))}</textarea></div>`;
}

function renderGenericModuleFields(config) {
  const primitiveRows = [];
  const complexBlocks = [];
  Object.entries(config || {}).forEach(([key, value]) => {
    if (Array.isArray(value)) {
      if (arrayEditors[key]) complexBlocks.push(renderArrayEditor(key, value, arrayEditors[key]));
      else complexBlocks.push(renderPrimitiveArray(key, value));
      return;
    }
    if (value && typeof value === 'object') {
      complexBlocks.push(renderObjectEditor(key, value));
      return;
    }
    primitiveRows.push(renderConfigSettingRow(key, value));
  });
  const basic = primitiveRows.length ? `<section class="setting-group card module-basic-settings">
    <h3><span class="icon">⚙</span> Основные настройки</h3>
    <div class="setting-list">${primitiveRows.join('')}</div>
  </section>` : '';
  const complex = complexBlocks.length ? `<div class="module-complex-settings">${complexBlocks.join('')}</div>` : '';
  return `<div class="module-settings-view">${basic}${complex}</div>`;
}

function renderWargmModuleFields(config) {
  const shopName = selectedShopModuleName();
  const primitiveRows = [];
  const complexBlocks = [];
  let rulesKey = 'Rules';
  let rulesValue = [];
  Object.entries(config || {}).forEach(([key, value]) => {
    if (String(key).toLowerCase() === 'rules') {
      rulesKey = key;
      rulesValue = Array.isArray(value) ? value : [];
      return;
    }
    if (Array.isArray(value)) {
      if (arrayEditors[key]) complexBlocks.push(renderArrayEditor(key, value, arrayEditors[key]));
      else complexBlocks.push(renderPrimitiveArray(key, value));
      return;
    }
    if (value && typeof value === 'object') {
      complexBlocks.push(renderObjectEditor(key, value));
      return;
    }
    primitiveRows.push(renderConfigSettingRow(key, value));
  });
  const active = state.wargmConfigTab === 'products' ? 'products' : 'settings';
  const settings = `<section class="setting-group card module-basic-settings">
    <h3><span class="icon">⚙</span> Основные настройки</h3>
    <div class="setting-list">${primitiveRows.join('') || '<div class="muted-line">Основных параметров пока нет.</div>'}</div>
  </section>
  ${complexBlocks.length ? `<div class="module-complex-settings">${complexBlocks.join('')}</div>` : ''}`;
  return `<div class="module-settings-view wargm-module-view">
    <div class="wargm-module-tabs" role="tablist" aria-label="Настройки ${escapeAttr(shopName)}">
      <button type="button" class="tab wargm-module-tab ${active === 'settings' ? 'active' : ''}" data-wargm-config-tab="settings" role="tab" aria-selected="${active === 'settings'}">Основные настройки</button>
      <button type="button" class="tab wargm-module-tab ${active === 'products' ? 'active' : ''}" data-wargm-config-tab="products" role="tab" aria-selected="${active === 'products'}">Товары <span>${Array.isArray(rulesValue) ? rulesValue.length : 0}</span></button>
    </div>
    <div class="wargm-module-tab-panel" data-wargm-config-panel="settings" ${active === 'settings' ? '' : 'hidden'}>${settings}</div>
    <div class="wargm-module-tab-panel wargm-products-panel" data-wargm-config-panel="products" ${active === 'products' ? '' : 'hidden'}>${renderWargmRulesEditor(rulesKey, rulesValue)}</div>
  </div>`;
}

const simpleModuleProfiles = {
  'welcome-pack': {
    key: 'welcome-pack',
    arrayKey: 'Items',
    itemsTab: 'Товары',
    listTitle: 'Предметы стартового набора',
    addLabel: 'Добавить предмет',
    modalKicker: 'Настройка предмета',
    modalSave: 'Сохранить предмет',
    emptyText: 'В стартовом наборе пока нет предметов.',
    searchPlaceholder: 'Поиск предмета: ID или количество',
    itemTitle: row => getNestedValue(row || {}, 'ItemId') || getNestedValue(row || {}, 'itemId') || 'Предмет без ID',
    itemSummary: row => `кол-во: ${getNestedValue(row || {}, 'Quantity') || getNestedValue(row || {}, 'quantity') || 1}`,
    fields: arrayEditors.Items,
    extraArrays: [{
      tab: 'vehicles',
      arrayKey: 'RentalVehicles',
      itemsTab: 'Транспорт 24ч',
      listTitle: 'Бесплатный транспорт стартпака',
      addLabel: 'Добавить транспорт',
      modalKicker: 'Настройка транспорта стартпака',
      modalSave: 'Сохранить транспорт',
      emptyText: 'Транспорт стартпака пока не настроен.',
      searchPlaceholder: 'Поиск транспорта: ID, название, штраф',
      cardMode: 'vehicle',
      itemTitle: row => getNestedValue(row || {}, 'DisplayName') || getNestedValue(row || {}, 'displayName') || getNestedValue(row || {}, 'AssetName') || getNestedValue(row || {}, 'assetName') || 'Транспорт без названия',
      itemSummary: row => {
        const asset = getNestedValue(row || {}, 'AssetName') || getNestedValue(row || {}, 'assetName') || '-';
        const minutes = Number(getNestedValue(row || {}, 'Minutes') || getNestedValue(row || {}, 'minutes') || 1440);
        const penalty = Number(getNestedValue(row || {}, 'MissingVehiclePenalty') || getNestedValue(row || {}, 'missingVehiclePenalty') || 0);
        return `${asset} · ${minutes} мин · штраф ${penalty}`;
      },
      fields: arrayEditors.WelcomeVehicles
    }],
    vipArrayKey: 'VipItems',
    vipItemsTab: 'VIP товары',
    vipListTitle: 'VIP предметы стартового набора',
    vipAddLabel: 'Добавить VIP предмет',
    vipModalKicker: 'Настройка VIP предмета',
    vipModalSave: 'Сохранить VIP предмет',
    vipEmptyText: 'VIP предметов в стартовом наборе пока нет.',
    vipSearchPlaceholder: 'Поиск VIP предмета: ID или количество',
    vipSettingsKey: 'VipSettings',
    vipSettingsTitle: 'Настройки VIP стартпака',
    vipSettingsSubtitle: 'Кому добавлять VIP-предметы и какие бонусы начислять вместе со стартовым набором.',
    vipSettingsFields: [
      ['Enabled', 'Включить VIP бонусы', 'checkbox'],
      ['RequiredPermission', 'VIP право', 'text'],
      ['CooldownHours', 'VIP кулдаун, ч', 'number'],
      ['MoneyAmount', 'Деньги', 'number'],
      ['GoldAmount', 'Золото', 'number'],
      ['FameAmount', 'Слава', 'number'],
      ['SuccessMessage', 'VIP сообщение', 'text']
    ],
    settingsFields: [
      ['Enabled', 'Включено', 'checkbox'],
      ['RequiredPermission', 'Требуемое право', 'text'],
      ['CooldownHours', 'Кулдаун, ч', 'number'],
      ['SerializeClaimsGlobally', 'Глобальная блокировка выдачи', 'checkbox'],
      ['InterItemDelayMs', 'Пауза между предметами, мс', 'number'],
      ['RentalVehicleMinutes', 'Транспорт стартпака, мин', 'number'],
      ['RentalVehicleSalePenalty', 'Штраф за продажу транспорта', 'number'],
      ['SuccessMessage', 'Сообщение при успехе', 'text']
    ]
  },
  'daily-pack': {
    key: 'daily-pack',
    arrayKey: 'Items',
    itemsTab: 'Товары',
    listTitle: 'Предметы ежедневного набора',
    addLabel: 'Добавить предмет',
    modalKicker: 'Настройка предмета',
    modalSave: 'Сохранить предмет',
    emptyText: 'В ежедневном наборе пока нет предметов.',
    searchPlaceholder: 'Поиск предмета: ID или количество',
    itemTitle: row => getNestedValue(row || {}, 'ItemId') || getNestedValue(row || {}, 'itemId') || 'Предмет без ID',
    itemSummary: row => `кол-во: ${getNestedValue(row || {}, 'Quantity') || getNestedValue(row || {}, 'quantity') || 1}`,
    fields: arrayEditors.Items,
    vipArrayKey: 'VipItems',
    vipItemsTab: 'VIP товары',
    vipListTitle: 'VIP предметы ежедневного набора',
    vipAddLabel: 'Добавить VIP предмет',
    vipModalKicker: 'Настройка VIP предмета',
    vipModalSave: 'Сохранить VIP предмет',
    vipEmptyText: 'VIP предметов в ежедневном наборе пока нет.',
    vipSearchPlaceholder: 'Поиск VIP предмета: ID или количество',
    vipSettingsKey: 'VipSettings',
    vipSettingsTitle: 'Настройки VIP дейлипака',
    vipSettingsSubtitle: 'VIP может иметь отдельный кулдаун, дополнительные предметы и денежные бонусы.',
    vipSettingsFields: [
      ['Enabled', 'Включить VIP бонусы', 'checkbox'],
      ['RequiredPermission', 'VIP право', 'text'],
      ['CooldownHours', 'VIP кулдаун, ч', 'number'],
      ['MoneyAmount', 'Деньги', 'number'],
      ['GoldAmount', 'Золото', 'number'],
      ['FameAmount', 'Слава', 'number'],
      ['SuccessMessage', 'VIP сообщение', 'text']
    ],
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['CooldownHours', 'Кулдаун, ч', 'number'],
      ['RequiredPermission', 'Требуемое право', 'text'],
      ['SuccessMessage', 'Сообщение при успехе', 'text']
    ]
  },
  'battlepass': {
    key: 'battlepass',
    arrayKey: 'Rewards',
    itemsTab: 'Дни',
    listTitle: 'Обычные награды Battlepass',
    addLabel: 'Добавить день',
    modalKicker: 'Настройка дня Battlepass',
    modalSave: 'Сохранить день',
    emptyText: 'Награды Battlepass пока не настроены.',
    searchPlaceholder: 'Поиск дня: номер, предмет, сумма',
    cardMode: 'battlepass-reward',
    cardPrefix: 'День',
    itemTitle: row => `День ${getNestedValue(row || {}, 'Day') || getNestedValue(row || {}, 'day') || '-'}`,
    itemSummary: row => {
      const money = Number(getNestedValue(row || {}, 'MoneyAmount') || getNestedValue(row || {}, 'moneyAmount') || 0);
      const gold = Number(getNestedValue(row || {}, 'GoldAmount') || getNestedValue(row || {}, 'goldAmount') || 0);
      const fame = Number(getNestedValue(row || {}, 'FameAmount') || getNestedValue(row || {}, 'fameAmount') || 0);
      const items = getNestedValue(row || {}, 'ItemsText') || getNestedValue(row || {}, 'itemsText') || '';
      return `$${money} · gold ${gold} · fame ${fame} · ${battlepassItemsSummary(items)}`;
    },
    fields: arrayEditors.BattlepassRewards,
    vipArrayKey: 'VipRewards',
    vipItemsTab: 'VIP дни',
    vipListTitle: 'VIP награды Battlepass',
    vipAddLabel: 'Добавить VIP день',
    vipModalKicker: 'Настройка VIP дня Battlepass',
    vipModalSave: 'Сохранить VIP день',
    vipEmptyText: 'VIP награды Battlepass пока не настроены.',
    vipSearchPlaceholder: 'Поиск VIP дня: номер, предмет, сумма',
    vipFields: arrayEditors.BattlepassRewards,
    vipSettingsKey: 'VipSettings',
    vipSettingsTitle: 'Настройки VIP Battlepass',
    vipSettingsSubtitle: 'VIP-дни настраиваются отдельной линейкой и применяются только к активным VIP-игрокам. Обычная линейка остаётся отдельной.',
    vipSettingsFields: [
      ['Enabled', 'Включить VIP Battlepass', 'checkbox'],
      ['RequiredPermission', 'VIP право', 'text'],
      ['SuccessMessage', 'VIP сообщение', 'text']
    ],
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['MaxDays', 'Максимум дней', 'number'],
      ['DelayAfterJoinSeconds', 'Задержка после входа, сек', 'number'],
      ['PollIntervalMs', 'Интервал проверки, мс', 'number'],
      ['RetryWindowSeconds', 'Окно повторов, сек', 'number'],
      ['InterItemDelayMs', 'Пауза между предметами, мс', 'number'],
      ['RequiredPermission', 'Требуемое право', 'text'],
      ['SuccessMessage', 'Сообщение при успехе', 'text']
    ]
  },
  'vip-system': {
    key: 'vip-system',
    arrayKey: 'Members',
    itemsTab: 'Игроки',
    listTitle: 'VIP игроки',
    addLabel: 'Добавить VIP',
    modalKicker: 'Настройка VIP',
    modalSave: 'Сохранить VIP',
    emptyText: 'VIP-игроков пока нет. Добавь SteamID и дату окончания.',
    searchPlaceholder: 'Поиск VIP: SteamID, ник, уровень',
    itemTitle: row => getNestedValue(row || {}, 'Name') || getNestedValue(row || {}, 'name') || getNestedValue(row || {}, 'SteamId') || getNestedValue(row || {}, 'steamId') || 'VIP без SteamID',
    itemSummary: row => {
      const steam = getNestedValue(row || {}, 'SteamId') || getNestedValue(row || {}, 'steamId') || 'SteamID не указан';
      const tier = getNestedValue(row || {}, 'Tier') || getNestedValue(row || {}, 'tier') || 'vip';
      const expiresRaw = getNestedValue(row || {}, 'ExpiresAtUtc') || getNestedValue(row || {}, 'expiresAtUtc') || getNestedValue(row || {}, 'ExpiresAt') || getNestedValue(row || {}, 'expiresAt');
      const expires = expiresRaw ? formatShortDateTime(expiresRaw) : 'срок не указан';
      return `${steam} / ${tier} / до ${expires}`;
    },
    fields: arrayEditors.Members,
    settingsFields: [
      ['Enabled', 'Включить VIP систему', 'checkbox'],
      ['DefaultTier', 'Уровень VIP по умолчанию', 'text'],
      ['DefaultDurationDays', 'Срок VIP по умолчанию, дней', 'number'],
      ['ExtendExisting', 'Продлевать текущий VIP', 'checkbox'],
      ['BasePermissions', 'Базовые права VIP', 'line-list'],
      ['Features.HomeSystem.Enabled', 'VIP дома включены', 'checkbox'],
      ['Features.HomeSystem.MaxHomes', 'Лимит домов VIP', 'number'],
      ['Features.WelcomePack.Enabled', 'VIP бонус стартпака', 'checkbox'],
      ['Features.DailyPack.Enabled', 'VIP бонус дейлипака', 'checkbox'],
      ['Features.DailyPack.CooldownHours', 'VIP кулдаун дейлипака, ч', 'number'],
      ['Features.Battlepass.Enabled', 'VIP Battlepass', 'checkbox'],
      ['Features.SectorScan.Enabled', 'VIP скан сектора', 'checkbox'],
      ['Features.SectorScan.Free', 'VIP скан бесплатно', 'checkbox'],
      ['Features.SectorScan.CooldownSeconds', 'VIP кулдаун скана, сек', 'number'],
      ['Features.VehicleRental.Enabled', 'VIP аренда транспорта', 'checkbox'],
      ['Features.VehicleRental.DiscountPercent', 'VIP скидка аренды, %', 'number'],
      ['Features.VehicleRental.DefaultMinutes', 'VIP аренда по умолчанию, мин', 'number'],
      ['Features.VehicleRental.MaxMinutes', 'VIP максимум аренды, мин', 'number'],
      ['Features.VehicleRental.SpawnCooldownSeconds', 'VIP кулдаун транспорта, сек', 'number'],
      ['Wargm.Enabled', 'VIP выдача через Wargm', 'checkbox'],
      ['Wargm.DefaultDurationDays', 'Wargm VIP дней по умолчанию', 'number'],
      ['Wargm.SuccessMessage', 'Сообщение Wargm VIP', 'text']
    ]
  },
  'fast-travel': {
    key: 'fast-travel',
    arrayKey: 'Outposts',
    itemsTab: 'Маршруты',
    listTitle: 'Маршруты быстрого перемещения',
    addLabel: 'Добавить маршрут',
    modalKicker: 'Настройка маршрута',
    modalSave: 'Сохранить маршрут',
    emptyText: 'Маршрутов пока нет.',
    searchPlaceholder: 'Поиск маршрута: название, команда, координаты',
    itemTitle: row => getNestedValue(row || {}, 'DisplayName') || getNestedValue(row || {}, 'displayName') || getNestedValue(row || {}, 'CommandAlias') || 'Маршрут без названия',
    itemSummary: row => {
      const alias = getNestedValue(row || {}, 'CommandAlias') || getNestedValue(row || {}, 'commandAlias') || '-';
      const price = getNestedValue(row || {}, 'Price') || getNestedValue(row || {}, 'price') || 0;
      return `команда: ${alias} · цена: ${price}`;
    },
    fields: arrayEditors.Outposts,
    settingsFields: [
      ['Enabled', 'Включено', 'checkbox'],
      ['FixedFare', 'Фиксированная цена, если маршрут без своей цены', 'number'],
      ['RatePerMeter', 'Цена за метр, если фиксированная цена 0', 'number'],
      ['TransferCooldownMinutes', 'Кулдаун перемещения, мин', 'number'],
      ['TeleportDelaySeconds', 'Задержка телепорта, сек', 'number'],
      ['CancelMoveDistance', 'Отмена при движении, см', 'number'],
      ['SuccessArrivalDistance', 'Радиус подтверждения прибытия, см', 'number'],
      ['MinArrivalZ', 'Безопасная высота прибытия Z', 'number']
    ]
  },
  'vehicle-rental': {
    key: 'vehicle-rental',
    arrayKey: 'Vehicles',
    itemsTab: 'Транспорт',
    listTitle: 'Транспорт для аренды',
    addLabel: 'Добавить транспорт',
    modalKicker: 'Настройка аренды',
    modalSave: 'Сохранить транспорт',
    emptyText: 'Транспорт для аренды пока не настроен.',
    searchPlaceholder: 'Поиск транспорта: команда, название, ID, цена',
    itemTitle: row => getNestedValue(row || {}, 'DisplayName') || getNestedValue(row || {}, 'displayName') || getNestedValue(row || {}, 'AssetName') || getNestedValue(row || {}, 'assetName') || 'Транспорт без названия',
    itemSummary: row => {
      const alias = getNestedValue(row || {}, 'Alias') || getNestedValue(row || {}, 'alias') || '-';
      const price = Number(getNestedValue(row || {}, 'PricePer10Minutes') || getNestedValue(row || {}, 'pricePer10Minutes') || 0);
      const start = Number(getNestedValue(row || {}, 'InitialCharge') || getNestedValue(row || {}, 'initialCharge') || 0);
      const minutes = Number(getNestedValue(row || {}, 'DefaultMinutes') || getNestedValue(row || {}, 'defaultMinutes') || 0);
      return `/${alias} · старт ${start} · ${price}/10 мин · ${minutes} мин`;
    },
    fields: arrayEditors.Vehicles,
    vipArrayKey: 'VipVehicles',
    vipItemsTab: 'VIP аренда',
    vipListTitle: 'VIP транспорт для аренды',
    vipAddLabel: 'Добавить VIP транспорт',
    vipModalKicker: 'Настройка VIP аренды',
    vipModalSave: 'Сохранить VIP транспорт',
    vipEmptyText: 'VIP транспорт пока не настроен. Обычные игроки не увидят эти варианты.',
    vipSearchPlaceholder: 'Поиск VIP транспорта: команда, название, ID, цена',
    vipFields: arrayEditors.Vehicles,
    settingsFields: [
      ['Enabled', 'Включено', 'checkbox'],
      ['DefaultRentalMinutes', 'Время по умолчанию, мин', 'number'],
      ['MinRentalMinutes', 'Мин. аренда, мин', 'number'],
      ['MaxRentalMinutes', 'Макс. аренда, мин', 'number'],
      ['ChargePenaltyOnMissingVehicle', 'Штраф за потерю', 'checkbox'],
      ['DefaultMissingVehiclePenalty', 'Штраф по умолчанию', 'number'],
      ['GlobalSpawnCooldownSeconds', 'Глобальный кулдаун спавна, сек', 'number'],
      ['PlayerSpawnCooldownSeconds', 'Кулдаун игрока, сек', 'number'],
      ['WarningMinutesBeforeExpiry', 'Предупреждения до конца, мин', 'number-list'],
      ['EnableRuntimeVehicleReturnDestroy', 'Удалять при возврате', 'checkbox'],
      ['EnableRuntimeVehicleExpireDestroy', 'Удалять по истечению', 'checkbox'],
      ['EnableRuntimeVehicleTrackAfterSpawn', 'Отслеживать после спавна', 'checkbox'],
      ['CleanupMaxPerRun', 'Очистка за запуск', 'number']
    ]
  },
  'money-transfer': {
    key: 'money-transfer',
    noItems: true,
    listTitle: 'Переводы денег',
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['RequiredPermission', 'Требуемое право', 'text'],
      ['Currency', 'Валюта', 'select:Normal|Gold'],
      ['MinAmount', 'Минимальная сумма', 'number'],
      ['MaxAmount', 'Максимальная сумма', 'number'],
      ['DailyLimit', 'Дневной лимит', 'number'],
      ['CooldownSeconds', 'Кулдаун, сек', 'number'],
      ['FeePercent', 'Комиссия, %', 'number'],
      ['FeeFixed', 'Фикс. комиссия', 'number'],
      ['RequireBothOnline', 'Оба игрока онлайн', 'checkbox'],
      ['AllowSelfTransfer', 'Разрешить перевод себе', 'checkbox'],
      ['SuccessMessage', 'Сообщение отправителю', 'text'],
      ['ReceivedMessage', 'Сообщение получателю', 'text'],
      ['UsageMessage', 'Подсказка формата', 'text']
    ]
  },
  'item-upgrade': {
    key: 'item-upgrade',
    arrayKey: 'Rules',
    itemsTab: 'Апгрейды',
    listTitle: 'Правила апгрейда предметов',
    addLabel: 'Добавить апгрейд',
    modalKicker: 'Настройка апгрейда',
    modalSave: 'Сохранить апгрейд',
    emptyText: 'Апгрейды пока не настроены.',
    searchPlaceholder: 'Поиск апгрейда: команда, предмет, цена',
    itemTitle: row => getNestedValue(row || {}, 'DisplayName') || getNestedValue(row || {}, 'Alias') || getNestedValue(row || {}, 'SourceItemId') || 'Апгрейд без названия',
    itemSummary: row => {
      const source = getNestedValue(row || {}, 'SourceItemId') || '-';
      const result = getNestedValue(row || {}, 'ResultItemId') || '-';
      const money = Number(getNestedValue(row || {}, 'CostMoney') || 0);
      const weight = Number(getNestedValue(row || {}, 'Weight') || getNestedValue(row || {}, 'SpawnWeight') || getNestedValue(row || {}, 'TargetWeight') || 0);
      const health = Number(getNestedValue(row || {}, 'Health') || 0);
      const mods = [weight > 0 ? `вес ${weight} кг` : '', health > 0 ? `HP ${health}%` : ''].filter(Boolean).join(' · ');
      return `${source} -> ${result}${mods ? ` · ${mods}` : ''} · ${money}`;
    },
    fields: arrayEditors.UpgradeRules,
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['AllowReplacement', 'Разрешить замену предметов', 'checkbox'],
      ['AllowSameItemId', 'Разрешить тот же ItemID с параметрами', 'checkbox'],
      ['RequiredPermission', 'Требуемое право', 'text'],
      ['CooldownSeconds', 'Кулдаун, сек', 'number'],
      ['RequireItemInHands', 'Требовать предмет в руках', 'checkbox'],
      ['SpawnResultNearPlayer', 'Выдавать рядом с игроком', 'checkbox'],
      ['SuccessMessage', 'Сообщение при успехе', 'text']
    ]
  },
  'base-loot-collector': {
    key: 'base-loot-collector',
    arrayKey: 'Rules',
    itemsTab: 'Правила',
    listTitle: 'Правила раскладки лута',
    addLabel: 'Добавить правило',
    modalKicker: 'Настройка правила',
    modalSave: 'Сохранить правило',
    emptyText: 'Правил раскладки пока нет. Без правил лут будет идти в сундук по умолчанию.',
    searchPlaceholder: 'Поиск правила: предмет, сундук, категория',
    itemTitle: row => getNestedValue(row || {}, 'Name') || getNestedValue(row || {}, 'ChestName') || 'Правило без названия',
    itemSummary: row => {
      const match = getNestedValue(row || {}, 'MatchContains') || '-';
      const chest = getNestedValue(row || {}, 'ChestName') || 'Loot';
      return `${chest} · ${String(match).slice(0, 64)}`;
    },
    fields: arrayEditors.BaseLootRules,
    vipSettingsKey: 'VipSettings',
    vipSettingsTitle: 'VIP сбор лута',
    vipSettingsSubtitle: 'VIP может иметь больший радиус, меньше кулдаун и больше предметов за один запуск.',
    vipSettingsFields: [
      ['Enabled', 'Включить VIP лимиты', 'checkbox'],
      ['RequiredPermission', 'VIP право', 'text'],
      ['RadiusCm', 'VIP радиус, см', 'number'],
      ['CooldownSeconds', 'VIP кулдаун, сек', 'number'],
      ['MaxItemsPerRun', 'VIP предметов за запуск', 'number']
    ],
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['RequiredPermission', 'Требуемое право', 'text'],
      ['RequireSquadOwnedFlag', 'Только отряд владельца флага', 'checkbox'],
      ['AllowFlagOwnerWithoutSquad', 'Разрешить владельцу без отряда', 'checkbox'],
      ['AllowAdminsBypass', 'Админ обходит проверку флага', 'checkbox'],
      ['RadiusCm', 'Радиус сбора, см', 'number'],
      ['CooldownSeconds', 'Кулдаун, сек', 'number'],
      ['MaxItemsPerRun', 'Предметов за запуск', 'number'],
      ['MaxChestsPerRun', 'Сундуков за запуск', 'number'],
      ['MaxScannedItems', 'Лимит скана предметов', 'number'],
      ['MaxScannedChests', 'Лимит скана сундуков', 'number'],
      ['CollectAllOnSingleCommand', 'Собирать всё за один вызов', 'checkbox'],
      ['BatchItemsPerTick', 'Предметов за тик', 'number'],
      ['BatchDelayMs', 'Пауза между пачками, мс', 'number'],
      ['MoveWorkBudgetMs', 'Бюджет пачки, мс', 'number'],
      ['SingleItemMoveBudgetMs', 'Бюджет предмета, мс', 'number'],
      ['CombinedItemScanLimit', 'Общий лимит item-scan', 'number'],
      ['InventoryLocationOccupancyBatch', 'Кэш занятых слотов за один scan', 'checkbox'],
      ['RequireOnFloorPresence', 'Только предметы на земле', 'checkbox'],
      ['NameContains', 'Искать сундук по части имени', 'checkbox'],
      ['DefaultChestName', 'Сундук по умолчанию', 'text'],
      ['FallbackToDefaultChest', 'Если нет сундука правила, использовать основной сундук', 'checkbox'],
      ['DryRun', 'Проверка без перемещения', 'checkbox'],
      ['SuccessMessage', 'Сообщение успеха', 'text'],
      ['EmptyMessage', 'Сообщение если пусто', 'text'],
      ['NoFlagMessage', 'Сообщение вне флага', 'text'],
      ['AccessDeniedMessage', 'Сообщение нет доступа', 'text'],
      ['DbUnavailableMessage', 'Сообщение нет SQLite/DB', 'text']
    ]
  },
  'help-response': {
    key: 'help-response',
    noItems: true,
    listTitle: 'Ответ команды /help',
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox', '', 'Если выключено, игра всегда использует стандартную динамическую справку.'],
      ['UseCustomText', 'Использовать свой текст вместо стандартного /help', 'checkbox', '', 'При выключении сохранённые строки не удаляются, но игроки видят динамическую справку.'],
      ['Text', 'Текст /help', 'chat-lines', '', 'Каждая строка — отдельное сообщение игрового чата.'],
      ['EnglishText', 'Текст /help для английского языка', 'chat-lines', '', 'Необязательно: если пусто, английский игрок увидит русскую версию.'],
      ['LineDelayMs', 'Пауза между строками, мс', 'number', '', '40–1000 мс применяются bridge-ом.'],
      ['MaxLines', 'Максимум строк', 'number', '', 'Панель и bridge ограничивают ответ 1–40 строками.'],
      ['MaxLineBytes', 'Максимум байт на строку', 'number', '', 'Не выше текущего лимита bridge (и не более 220 байт).']
    ]
  },
  'info-response': {
    key: 'info-response',
    noItems: true,
    listTitle: 'Ответ команды /info',
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox', '', 'Если выключено, игра всегда использует стандартное динамическое описание команд.'],
      ['UseCustomText', 'Использовать свой текст вместо стандартного /info', 'checkbox', '', 'При выключении сохранённые строки не удаляются, но игроки видят динамическое описание команд.'],
      ['Text', 'Текст /info', 'chat-lines', '', 'Каждая строка — отдельное сообщение игрового чата.'],
      ['EnglishText', 'Текст /info для английского языка', 'chat-lines', '', 'Необязательно: если пусто, английский игрок увидит русскую версию.'],
      ['LineDelayMs', 'Пауза между строками, мс', 'number', '', '40–1000 мс применяются bridge-ом.'],
      ['MaxLines', 'Максимум строк', 'number', '', 'Панель и bridge ограничивают ответ 1–40 строками.'],
      ['MaxLineBytes', 'Максимум байт на строку', 'number', '', 'Не выше текущего лимита bridge (и не более 220 байт).']
    ]
  },
  'command-aliases': {
    key: 'command-aliases',
    noItems: true,
    listTitle: 'Команды игроков',
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['IncludeBangAliases', 'Также принимать !команды', 'checkbox'],
      ['Help', 'Помощь', 'line-list', '', 'Например: helper'],
      ['Info', 'Описание команд', 'line-list', '', 'Например: info'],
      ['Hello', 'Проверка связи', 'line-list'],
      ['Language', 'Язык', 'line-list'],
      ['SetHome', 'Установить дом', 'line-list'],
      ['Home', 'Телепорт домой', 'line-list'],
      ['Homes', 'Список домов', 'line-list'],
      ['DeleteHome', 'Удалить дом', 'line-list'],
      ['PrivateMessage', 'Личное сообщение', 'line-list'],
      ['Reply', 'Ответить', 'line-list'],
      ['PrivateMessageHistory', 'История ЛС', 'line-list'],
      ['SectorScan', 'Скан сектора', 'line-list'],
      ['FastTravel', 'Быстрое перемещение', 'line-list'],
      ['BaseLoot', 'Сбор лута', 'line-list'],
      ['Rent', 'Аренда транспорта', 'line-list', '', 'Например: arenda'],
      ['WelcomePack', 'Стартовый набор', 'line-list'],
      ['DailyPack', 'Ежедневный набор', 'line-list'],
      ['Battlepass', 'Battlepass', 'line-list'],
      ['Dlc', 'DLC unlock', 'line-list'],
      ['GameStores', 'GameStores / получение покупок', 'line-list'],
      ['MoneyTransfer', 'Перевод денег', 'line-list'],
      ['ItemUpgrade', 'Апгрейд предметов', 'line-list'],
      ['Quest', 'Квесты', 'line-list'],
      ['Streak', 'Серия убийств', 'line-list'],
      ['Bounty', 'Bounty', 'line-list'],
      ['HunterTop', 'Топ охотников', 'line-list'],
      ['Wargm', 'Wargm', 'line-list'],
      ['InventoryDelete', 'Удаление предмета', 'line-list'],
      ['Vip', 'VIP', 'line-list'],
      ['DiscordTest', 'Discord тест', 'line-list'],
      ['Armory', 'Armory', 'line-list'],
      ['ArmoryBack', 'Возврат Armory', 'line-list'],
      ['Editor', 'Редактор', 'line-list']
    ]
  },
  'zone-robot-schedule': {
    key: 'zone-robot-schedule',
    arrayKey: 'Rules',
    itemsTab: 'Правила',
    listTitle: 'Расписание зонных команд',
    addLabel: 'Добавить правило',
    modalKicker: 'Настройка зоны',
    modalSave: 'Сохранить правило',
    emptyText: 'Правила зон пока не настроены.',
    searchPlaceholder: 'Поиск зоны: сектор, время, команда',
    itemTitle: row => getNestedValue(row || {}, 'Name') || getNestedValue(row || {}, 'Zone') || 'Правило без названия',
    itemSummary: row => {
      const zone = getNestedValue(row || {}, 'Zone') || '-';
      const on = getNestedValue(row || {}, 'ScheduleTimesOn') || '-';
      const off = getNestedValue(row || {}, 'ScheduleTimesOff') || '-';
      return `${zone} · вкл ${on} · выкл ${off}`;
    },
    fields: arrayEditors.ZoneRobotRules,
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['PollIntervalMs', 'Интервал проверки, мс', 'number']
    ]
  },
  'panel-quests': {
    key: 'panel-quests',
    arrayKey: 'Quests',
    itemsTab: 'Квесты',
    listTitle: 'Квестовая доска сервера',
    addLabel: 'Добавить квест',
    modalKicker: 'Настройка квеста',
    modalSave: 'Сохранить квест',
    emptyText: 'Квестов пока нет.',
    searchPlaceholder: 'Поиск квеста: команда, название, награда',
    itemTitle: row => getNestedValue(row || {}, 'Title') || getNestedValue(row || {}, 'Alias') || 'Квест без названия',
    itemSummary: row => {
      const alias = getNestedValue(row || {}, 'Alias') || '-';
      const mode = getNestedValue(row || {}, 'Mode') || 'Claim';
      const money = Number(getNestedValue(row || {}, 'RewardMoney') || 0);
      return `/${alias} · ${mode} · ${money}`;
    },
    fields: arrayEditors.QuestRules,
    settingsFields: [
      ['Name', 'Название', 'text'],
      ['Description', 'Описание', 'text'],
      ['Enabled', 'Включено', 'checkbox'],
      ['RequiredPermission', 'Требуемое право', 'text'],
      ['ClaimCooldownHours', 'Кулдаун claim, ч', 'number'],
      ['AnnounceOnClaim', 'Объявлять выполнение', 'checkbox'],
      ['ListMessage', 'Сообщение списка', 'text']
    ]
  }
};

function simpleModuleProfile(key = state.selectedModule) {
  return simpleModuleProfiles[String(key || '').toLowerCase()] || null;
}

function simpleModuleArrayKey(config, profile) {
  const preferred = profile && profile.arrayKey ? profile.arrayKey : 'Items';
  return Object.keys(config || {}).find(key => key.toLowerCase() === preferred.toLowerCase()) || preferred;
}

function simpleModuleHasVipItems(profile) {
  return !!(profile && profile.vipArrayKey);
}

function simpleModuleVipArrayKey(config, profile) {
  const preferred = profile && profile.vipArrayKey ? profile.vipArrayKey : 'VipItems';
  return Object.keys(config || {}).find(key => key.toLowerCase() === preferred.toLowerCase()) || preferred;
}

function simpleModuleVipSettingsKey(config, profile) {
  const preferred = profile && profile.vipSettingsKey ? profile.vipSettingsKey : 'VipSettings';
  return Object.keys(config || {}).find(key => key.toLowerCase() === preferred.toLowerCase()) || preferred;
}

function simpleModuleExtraArrays(profile) {
  return Array.isArray(profile && profile.extraArrays) ? profile.extraArrays : [];
}

function simpleModuleExtraProfile(profile, tab) {
  return simpleModuleExtraArrays(profile).find(entry => String(entry.tab || '').toLowerCase() === String(tab || '').toLowerCase()) || null;
}

function simpleModuleExtraArrayKey(config, extra) {
  const preferred = extra && extra.arrayKey ? extra.arrayKey : 'Items';
  return Object.keys(config || {}).find(key => key.toLowerCase() === preferred.toLowerCase()) || preferred;
}

function simpleModuleAllowedTabs(profile) {
  const tabs = ['settings'];
  if (!profile || profile.noItems !== true) tabs.push('items');
  if (simpleModuleHasVipItems(profile)) tabs.push('vip-items');
  simpleModuleExtraArrays(profile).forEach(extra => {
    const tab = String(extra.tab || '').trim();
    if (tab) tabs.push(tab);
  });
  return tabs;
}

function simpleModuleTab(profile) {
  const key = profile && profile.key ? profile.key : state.selectedModule || '';
  const current = state.simpleModuleTabs[key] || 'settings';
  if (current === 'items' && (!profile || profile.noItems !== true)) return 'items';
  if (current === 'vip-items' && simpleModuleHasVipItems(profile)) return 'vip-items';
  if (simpleModuleExtraProfile(profile, current)) return current;
  return 'settings';
}

function simpleModuleRows(config, arrayKey) {
  return Array.isArray((config || {})[arrayKey]) ? (config || {})[arrayKey] : [];
}

function simpleModuleTabArrayKey(config, profile, tab = simpleModuleTab(profile)) {
  if (tab === 'vip-items' && simpleModuleHasVipItems(profile)) return simpleModuleVipArrayKey(config || {}, profile);
  const extra = simpleModuleExtraProfile(profile, tab);
  if (extra) return simpleModuleExtraArrayKey(config || {}, extra);
  return simpleModuleArrayKey(config || {}, profile);
}

function simpleModuleListProfile(profile, mode) {
  const extra = simpleModuleExtraProfile(profile, mode);
  if (extra) {
    return Object.assign({}, profile, extra, {
      fields: extra.fields || profile.fields || arrayEditors.Items,
      itemTitle: extra.itemTitle || profile.itemTitle,
      itemSummary: extra.itemSummary || profile.itemSummary
    });
  }
  if (mode !== 'vip-items') return profile;
  return Object.assign({}, profile, {
    itemsTab: profile.vipItemsTab || 'VIP товары',
    listTitle: profile.vipListTitle || 'VIP товары',
    addLabel: profile.vipAddLabel || 'Добавить VIP товар',
    modalKicker: profile.vipModalKicker || 'Настройка VIP товара',
    modalSave: profile.vipModalSave || 'Сохранить VIP товар',
    emptyText: profile.vipEmptyText || 'VIP товары пока не настроены.',
    searchPlaceholder: profile.vipSearchPlaceholder || 'Поиск VIP товара',
    fields: profile.vipFields || profile.fields || arrayEditors.Items
  });
}

function simpleModuleItemText(row, index, profile) {
  return [
    index + 1,
    profile.itemTitle(row),
    profile.itemSummary(row),
    JSON.stringify(row || {})
  ].join(' ').toLowerCase();
}

function isPackItemsProfile(profile) {
  return profile && profile.cardMode !== 'vehicle' && profile.cardMode !== 'welcome-vehicle' && (profile.key === 'welcome-pack' || profile.key === 'daily-pack');
}

function isVipMembersProfile(profile) {
  return profile && profile.key === 'vip-system';
}

function isVehicleRentalProfile(profile) {
  return profile && (profile.key === 'vehicle-rental' || profile.cardMode === 'vehicle' || profile.cardMode === 'welcome-vehicle');
}

function isBattlepassRewardsProfile(profile) {
  return profile && (profile.key === 'battlepass' || profile.cardMode === 'battlepass-reward');
}

function normalizePackCategoryLabel(value) {
  const raw = String(value || '').trim();
  const lower = raw.toLowerCase();
  if (!raw) return '';
  if (/weapon|rifle|shotgun|pistol|bow|crossbow|melee|оруж/i.test(lower)) return 'Оружие';
  if (/magazine|clip|магаз/i.test(lower)) return 'Магазин';
  if (/(^|[^a-zа-я])cal([^a-zа-я]|$)|ammo|ammunition|bullet(?!proof)|cartridge|shell|патрон/i.test(lower)) return 'Патроны';
  if (/clothing|gear|equipment|armor|armour|helmet|vest|backpack|boots|gloves|pants|jacket|экип|одеж|брон|шлем|рюкзак/i.test(lower)) return 'Экипировка';
  if (/medical|medicine|health|bandage|мед|бинт/i.test(lower)) return 'Медицина';
  if (/food|drink|provision|water|еда|вода|провиз/i.test(lower)) return 'Провизия';
  return raw;
}

function inferPackItemCategory(itemId, catalogItem) {
  const id = String(itemId || '').toLowerCase();
  if (/weapon|rifle|shotgun|pistol|bow|crossbow|sword|knife|bayonet|grenade/.test(id)) return 'Оружие';
  if (/magazine|clip/.test(id)) return 'Магазин';
  if (/(^|_)cal[_-]|ammo|ammunition|bullet(?!proof)|cartridge|shell|arrow/.test(id)) return 'Патроны';
  if (/backpack|bag|vest|armor|armour|helmet|jacket|pants|boots|gloves|shirt|mask/.test(id)) return 'Экипировка';
  if (/bandage|medical|painkiller|vitamin|antibiotic|syringe|pill/.test(id)) return 'Медицина';
  if (/water|mre|food|meat|tuna|apple|drink|can|milk/.test(id)) return 'Провизия';
  const explicit = normalizePackCategoryLabel(catalogItem && (catalogItem.category || catalogItem.Category || catalogItem.type || catalogItem.Type));
  if (explicit) return explicit;
  return 'Items';
}

function packItemCategoryClass(category) {
  const value = String(category || '').toLowerCase();
  if (/оруж|weapon|rifle|pistol|shotgun|melee/.test(value)) return 'pack-cat-weapon';
  if (/патрон|ammo|magazine|магаз|bullet(?!proof)|cartridge|shell/.test(value)) return 'pack-cat-ammo';
  if (/экип|clothing|gear|armor|helmet|vest|backpack|boots|gloves/.test(value)) return 'pack-cat-gear';
  if (/мед|medical|bandage|health/.test(value)) return 'pack-cat-medical';
  if (/провиз|food|drink|water/.test(value)) return 'pack-cat-food';
  return 'pack-cat-misc';
}

function isGeneratedIconUrl(url) {
  return /^data:image\/svg/i.test(String(url || ''));
}

function packItemIconUrl(itemId, catalogItem, category) {
  const direct = getItemIconUrl(itemId) || (catalogItem ? getCatalogIconUrl(catalogItem, 'item') : '');
  if (direct) return direct;
  return generatedCatalogIconUrl(Object.assign({ itemId, category, name: itemId }, catalogItem || {}), 'item');
}

function catalogThumbHtml(value, kind = 'item', options = {}) {
  const id = String(value || '').trim();
  const catalogItem = id ? findCatalogMatch(id, kind) : null;
  const title = catalogItem ? catalogDisplayName(catalogItem, kind) : friendlyAssetName(id, kind);
  const category = kind === 'vehicle' ? 'Транспорт' : inferPackItemCategory(id, catalogItem);
  let iconUrl = '';
  if (id) {
    iconUrl = getItemIconUrl(id) || (catalogItem ? getCatalogIconUrl(catalogItem, kind) : '');
    if (!iconUrl && options.allowGenerated) {
      iconUrl = kind === 'vehicle'
        ? generatedCatalogIconUrl(Object.assign({ vehicleId: id, category, name: title || id }, catalogItem || {}), 'vehicle')
        : generatedCatalogIconUrl(Object.assign({ itemId: id, category, name: title || id }, catalogItem || {}), 'item');
    }
  }
  const iconClass = iconUrl ? (isGeneratedIconUrl(iconUrl) ? 'has-generated-icon' : 'has-real-icon') : 'has-no-icon';
  const extraClass = options.className ? ` ${options.className}` : '';
  const attrs = options.attrs ? ` ${options.attrs}` : '';
  const fallback = kind === 'vehicle' ? 'VH' : 'IT';
  return `<div class="wargm-item-thumb pack-item-thumb catalog-thumb ${escapeAttr(kind)} ${iconClass}${extraClass}" title="${escapeAttr(title || id || fallback)}" aria-hidden="true"${attrs}>
    ${iconUrl ? `<img src="${escapeAttr(iconUrl)}" alt="" loading="lazy" />` : ''}
    <span>${escapeHtml(shortCode(id || title || fallback, fallback))}</span>
  </div>`;
}

function renderSimplePackItemCard(profile, arrayKey, row, index, matches) {
  const itemId = String(getNestedValue(row || {}, 'ItemId') || getNestedValue(row || {}, 'itemId') || '').trim();
  const quantity = Math.max(1, Number(getNestedValue(row || {}, 'Quantity') || getNestedValue(row || {}, 'quantity') || 1));
  const catalogItem = itemId ? findCatalogMatch(itemId, 'item') : null;
  const category = inferPackItemCategory(itemId, catalogItem);
  const iconUrl = packItemIconUrl(itemId || 'Item', catalogItem, category);
  const iconClass = iconUrl ? (isGeneratedIconUrl(iconUrl) ? 'has-generated-icon' : 'has-real-icon') : 'has-no-icon';
  const categoryClass = packItemCategoryClass(category);
  const label = catalogItem ? (catalogItem.name || catalogItem.displayName || '') : '';
  const title = itemId || 'Предмет без ID';
  const searchText = simpleModuleItemText(row, index, profile);
  return `<article class="wargm-item-card pack-item-card simple-module-card ${categoryClass} ${state.simpleModuleEditIndex === index && state.simpleModuleEditArrayKey === arrayKey ? 'active' : ''}" data-simple-item-card="true" data-simple-item-array-key="${escapeAttr(arrayKey)}" data-simple-item-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
    <div class="wargm-item-thumb pack-item-thumb ${iconClass}" aria-hidden="true">
      ${iconUrl ? `<img src="${escapeAttr(iconUrl)}" alt="" loading="lazy" />` : ''}
      <span>${escapeHtml(shortCode(itemId || 'IT'))}</span>
    </div>
    <div class="wargm-item-info pack-item-info">
      <div class="wargm-item-title pack-item-title" title="${escapeAttr(title)}">${escapeHtml(title)}</div>
      ${label && label !== title ? `<div class="pack-item-label">${escapeHtml(label)}</div>` : ''}
      <div class="wargm-item-category pack-item-category">${escapeHtml(category)}</div>
    </div>
    <div class="wargm-item-pricing pack-item-metrics">
      <div class="wargm-price-box pack-qty-box">
        <label>Кол-во</label>
        <strong>${escapeHtml(quantity)}</strong>
      </div>
      <span class="wargm-rule-card-status on pack-config-status">в конфиге</span>
    </div>
    <div class="pack-item-actions">
      <button type="button" class="mini-action primary pack-item-config" data-simple-item-open="${index}" data-simple-item-array-key="${escapeAttr(arrayKey)}">Настроить</button>
      <button type="button" class="wargm-item-delete pack-item-delete" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}" title="Удалить товар" aria-label="Удалить товар">Удалить</button>
    </div>
  </article>`;
}

function renderSimpleVipMemberCard(profile, arrayKey, row, index, matches) {
  const title = profile.itemTitle(row, index);
  const steam = getNestedValue(row || {}, 'SteamId') || getNestedValue(row || {}, 'steamId') || 'SteamID не указан';
  const tier = getNestedValue(row || {}, 'Tier') || getNestedValue(row || {}, 'tier') || 'vip';
  const note = getNestedValue(row || {}, 'Note') || getNestedValue(row || {}, 'note') || '';
  const enabled = getNestedValue(row || {}, 'Enabled') !== false && getNestedValue(row || {}, 'enabled') !== false;
  const expiresRaw = getNestedValue(row || {}, 'ExpiresAtUtc') || getNestedValue(row || {}, 'expiresAtUtc') || getNestedValue(row || {}, 'ExpiresAt') || getNestedValue(row || {}, 'expiresAt') || '';
  const expires = String(expiresRaw || 'срок не указан').replace('T', ' ').replace(/:\d{2}(?:\.\d+)?Z?$/, '');
  const searchText = simpleModuleItemText(row, index, profile);
  return `<article class="wargm-item-card pack-item-card vip-member-card simple-module-card ${state.simpleModuleEditIndex === index && state.simpleModuleEditArrayKey === arrayKey ? 'active' : ''}" data-simple-item-card="true" data-simple-item-array-key="${escapeAttr(arrayKey)}" data-simple-item-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
    <div class="wargm-item-thumb pack-item-thumb vip-member-thumb" aria-hidden="true">
      <span>${escapeHtml(shortCode(title || steam, 'VIP'))}</span>
    </div>
    <div class="wargm-item-info pack-item-info vip-member-info">
      <div class="wargm-item-title pack-item-title" title="${escapeAttr(title)}">${escapeHtml(title)}</div>
      <div class="pack-item-label">${escapeHtml(steam)}</div>
      <div class="vip-member-tags">
        <span class="wargm-item-category pack-item-category">${escapeHtml(tier)}</span>
        ${note ? `<span class="vip-member-note">${escapeHtml(note)}</span>` : ''}
      </div>
    </div>
    <div class="wargm-item-pricing pack-item-metrics vip-member-metrics">
      <div class="wargm-price-box pack-qty-box vip-date-box">
        <label>До</label>
        <strong>${escapeHtml(expires)}</strong>
      </div>
      <span class="wargm-rule-card-status ${enabled ? 'on' : ''} pack-config-status">${enabled ? 'активен' : 'выкл'}</span>
      <span class="wargm-rule-card-status on pack-config-status">в конфиге</span>
    </div>
    <div class="pack-item-actions vip-member-actions">
      <button type="button" class="mini-action primary pack-item-config" data-simple-item-open="${index}" data-simple-item-array-key="${escapeAttr(arrayKey)}">Настроить</button>
      <button type="button" class="wargm-item-delete pack-item-delete" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}" title="Удалить VIP" aria-label="Удалить VIP">Удалить</button>
    </div>
  </article>`;
}

function renderSimpleVehicleRentalCard(profile, arrayKey, row, index, matches) {
  const title = profile.itemTitle(row, index);
  const asset = String(getNestedValue(row || {}, 'AssetName') || getNestedValue(row || {}, 'assetName') || '').trim();
  const alias = String(getNestedValue(row || {}, 'Alias') || getNestedValue(row || {}, 'alias') || '').trim();
  const price = Math.max(0, Number(getNestedValue(row || {}, 'PricePer10Minutes') || getNestedValue(row || {}, 'pricePer10Minutes') || 0));
  const start = Math.max(0, Number(getNestedValue(row || {}, 'InitialCharge') || getNestedValue(row || {}, 'initialCharge') || 0));
  const minutes = Math.max(0, Number(getNestedValue(row || {}, 'DefaultMinutes') || getNestedValue(row || {}, 'defaultMinutes') || getNestedValue(row || {}, 'Minutes') || getNestedValue(row || {}, 'minutes') || 0));
  const maxMinutes = Math.max(0, Number(getNestedValue(row || {}, 'MaxMinutes') || getNestedValue(row || {}, 'maxMinutes') || 0));
  const penalty = Math.max(0, Number(getNestedValue(row || {}, 'MissingVehiclePenalty') || getNestedValue(row || {}, 'missingVehiclePenalty') || 0));
  const catalogItem = asset ? findCatalogMatch(asset, 'vehicle') : null;
  const iconUrl = catalogItem ? (getCatalogIconUrl(catalogItem, 'vehicle') || generatedCatalogIconUrl(catalogItem, 'vehicle')) : generatedCatalogIconUrl({ vehicleId: asset, name: title, category: 'Транспорт' }, 'vehicle');
  const searchText = simpleModuleItemText(row, index, profile);
  return `<article class="wargm-item-card pack-item-card rental-vehicle-card simple-module-card ${state.simpleModuleEditIndex === index && state.simpleModuleEditArrayKey === arrayKey ? 'active' : ''}" data-simple-item-card="true" data-simple-item-array-key="${escapeAttr(arrayKey)}" data-simple-item-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
    <div class="wargm-item-thumb pack-item-thumb rental-vehicle-thumb" aria-hidden="true">
      ${iconUrl ? `<img src="${escapeAttr(iconUrl)}" alt="" loading="lazy" />` : ''}
      <span>${escapeHtml(shortCode(title || asset || 'VR'))}</span>
    </div>
    <div class="wargm-item-info pack-item-info rental-vehicle-info">
      <div class="wargm-item-title pack-item-title" title="${escapeAttr(title)}">${escapeHtml(title)}</div>
      <div class="pack-item-label">${escapeHtml(asset || 'ID транспорта не указан')}</div>
      <div class="vip-member-tags">
        <span class="wargm-item-category pack-item-category">${escapeHtml(alias ? `/${alias}` : 'команда не задана')}</span>
        <span class="vip-member-note">штраф: ${escapeHtml(penalty)}</span>
      </div>
    </div>
    <div class="wargm-item-pricing pack-item-metrics rental-vehicle-metrics">
      <div class="wargm-price-box pack-qty-box rental-price-box">
        <label>Старт</label>
        <strong>${escapeHtml(start)}</strong>
      </div>
      <div class="wargm-price-box pack-qty-box rental-price-box">
        <label>10 мин</label>
        <strong>${escapeHtml(price)}</strong>
      </div>
      <div class="wargm-price-box pack-qty-box rental-price-box">
        <label>Время</label>
        <strong>${escapeHtml(minutes || maxMinutes || '-')}</strong>
      </div>
    </div>
    <div class="pack-item-actions rental-vehicle-actions">
      <button type="button" class="mini-action primary pack-item-config" data-simple-item-open="${index}" data-simple-item-array-key="${escapeAttr(arrayKey)}">Настроить</button>
      <button type="button" class="wargm-item-delete pack-item-delete" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}" title="Удалить транспорт" aria-label="Удалить транспорт">Удалить</button>
    </div>
  </article>`;
}

function renderBattlepassRewardCard(profile, arrayKey, row, index, matches) {
  const day = Math.max(1, Number(getNestedValue(row || {}, 'Day') || getNestedValue(row || {}, 'day') || index + 1));
  const enabled = getNestedValue(row || {}, 'Enabled') !== false && getNestedValue(row || {}, 'enabled') !== false;
  const money = Number(getNestedValue(row || {}, 'MoneyAmount') || getNestedValue(row || {}, 'moneyAmount') || getNestedValue(row || {}, 'Money') || 0);
  const gold = Number(getNestedValue(row || {}, 'GoldAmount') || getNestedValue(row || {}, 'goldAmount') || getNestedValue(row || {}, 'Gold') || 0);
  const fame = Number(getNestedValue(row || {}, 'FameAmount') || getNestedValue(row || {}, 'fameAmount') || getNestedValue(row || {}, 'Fame') || 0);
  const itemsText = String(getNestedValue(row || {}, 'ItemsText') || getNestedValue(row || {}, 'itemsText') || getNestedValue(row || {}, 'ItemsSpec') || getNestedValue(row || {}, 'itemsSpec') || '').trim();
  const firstItem = (itemsText.split(';').map(part => part.split('|')[0].trim()).find(Boolean)) || '';
  const title = `${profile.cardPrefix || 'День'} ${day}`;
  const searchText = simpleModuleItemText(row, index, profile);
  return `<article class="wargm-item-card pack-item-card battlepass-reward-card simple-module-card ${state.simpleModuleEditIndex === index && state.simpleModuleEditArrayKey === arrayKey ? 'active' : ''}" data-simple-item-card="true" data-simple-item-array-key="${escapeAttr(arrayKey)}" data-simple-item-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
    ${firstItem ? catalogThumbHtml(firstItem, 'item', { className: 'battlepass-reward-thumb', allowGenerated: true }) : `<div class="wargm-item-thumb pack-item-thumb battlepass-reward-thumb has-no-icon" aria-hidden="true"><span>${escapeHtml(String(day))}</span></div>`}
    <div class="wargm-item-info pack-item-info">
      <div class="wargm-item-title pack-item-title" title="${escapeAttr(title)}">${escapeHtml(title)}</div>
      <div class="pack-item-label">${escapeHtml(battlepassItemsSummary(itemsText))}</div>
      <div class="vip-member-tags">
        <span class="wargm-item-category pack-item-category">${money ? `$${money}` : 'деньги 0'}</span>
        <span class="vip-member-note">gold ${gold}</span>
        <span class="vip-member-note">fame ${fame}</span>
      </div>
    </div>
    <div class="wargm-item-pricing pack-item-metrics">
      <span class="wargm-rule-card-status on pack-config-status">${escapeHtml(arrayKey.toLowerCase().includes('vip') ? 'VIP' : 'обычный')}</span>
    </div>
    <div class="pack-item-actions">
      <button type="button" class="mini-action primary pack-item-config" data-simple-item-open="${index}" data-simple-item-array-key="${escapeAttr(arrayKey)}">Настроить</button>
      <button type="button" class="wargm-item-delete pack-item-delete" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}" title="Удалить день" aria-label="Удалить день">Удалить</button>
    </div>
  </article>`;
}

function renderUpgradeRuleCard(profile, arrayKey, row, index, matches) {
  const title = profile.itemTitle(row, index);
  const source = String(getNestedValue(row || {}, 'SourceItemId') || '').trim();
  const result = String(getNestedValue(row || {}, 'ResultItemId') || '').trim();
  const enabled = getNestedValue(row || {}, 'Enabled') !== false && getNestedValue(row || {}, 'enabled') !== false;
  const weight = Number(getNestedValue(row || {}, 'Weight') || getNestedValue(row || {}, 'SpawnWeight') || getNestedValue(row || {}, 'TargetWeight') || 0);
  const health = Number(getNestedValue(row || {}, 'Health') || 0);
  const money = Math.max(0, Number(getNestedValue(row || {}, 'CostMoney') || 0));
  const gold = Math.max(0, Number(getNestedValue(row || {}, 'CostGold') || 0));
  const fame = Math.max(0, Number(getNestedValue(row || {}, 'CostFame') || 0));
  const alias = String(getNestedValue(row || {}, 'Alias') || '').trim();
  const searchText = simpleModuleItemText(row, index, profile);
  const cost = [money ? `$${money}` : '', gold ? `${gold} gold` : '', fame ? `${fame} fame` : ''].filter(Boolean).join(' · ') || 'бесплатно';
  return `<article class="wargm-item-card pack-item-card upgrade-rule-card simple-module-card ${state.simpleModuleEditIndex === index && state.simpleModuleEditArrayKey === arrayKey ? 'active' : ''}" data-simple-item-card="true" data-simple-item-array-key="${escapeAttr(arrayKey)}" data-simple-item-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
    ${catalogThumbHtml(result || source, 'item', { className: 'upgrade-rule-thumb', allowGenerated: true })}
    <div class="wargm-item-info pack-item-info upgrade-rule-info">
      <div class="wargm-item-title pack-item-title" title="${escapeAttr(title)}">${escapeHtml(title)}</div>
      <div class="pack-item-label" title="${escapeAttr(`${source} -> ${result}`)}">${escapeHtml(source || 'предмет в руках не задан')} -> ${escapeHtml(result || 'результат не задан')}</div>
      <div class="vip-member-tags">
        <span class="wargm-item-category pack-item-category">${escapeHtml(alias ? `/${alias}` : 'команда не задана')}</span>
        ${weight > 0 ? `<span class="vip-member-note">вес ${escapeHtml(weight)} кг</span>` : ''}
        ${health > 0 ? `<span class="vip-member-note">HP ${escapeHtml(health)}%</span>` : ''}
      </div>
    </div>
    <div class="wargm-item-pricing pack-item-metrics upgrade-rule-metrics">
      <div class="wargm-price-box pack-qty-box">
        <label>Цена</label>
        <strong>${escapeHtml(cost)}</strong>
      </div>
    </div>
    <div class="pack-item-actions upgrade-rule-actions">
      <button type="button" class="mini-action primary pack-item-config" data-simple-item-open="${index}" data-simple-item-array-key="${escapeAttr(arrayKey)}">Редактировать</button>
      <button type="button" class="wargm-item-delete pack-item-delete" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}" title="Удалить апгрейд" aria-label="Удалить апгрейд">Удалить</button>
    </div>
  </article>`;
}

function renderSimpleModuleItemModal(profile, arrayKey, index, row) {
  const fields = profile.fields || arrayEditors[arrayKey] || arrayEditors[arrayKey.toLowerCase()] || [];
  return `<div class="wargm-rule-modal simple-module-modal" data-simple-item-overlay="true" role="dialog" aria-modal="true">
    <div class="wargm-rule-modal-card simple-module-modal-card">
      <div class="wargm-rule-modal-head">
        <div>
          <span class="wargm-modal-kicker">${escapeHtml(profile.modalKicker)}</span>
          <h3>${escapeHtml(profile.itemTitle(row, index))}</h3>
          <p>${escapeHtml(profile.itemSummary(row, index))}</p>
        </div>
        <div class="wargm-rule-modal-actions">
          <span class="wargm-mode-pill">${escapeHtml(profile.itemsTab)}</span>
          <button type="button" class="mini-action primary" data-simple-item-save="true">${escapeHtml(profile.modalSave)}</button>
          <button type="button" class="mini-action" data-simple-item-close="true">Закрыть</button>
        </div>
      </div>
      <div class="wargm-rule-layout simple-module-modal-body">
        <section class="wargm-rule-section">
          <div class="wargm-rule-section-title">
            <div><h4>Параметры</h4></div>
          </div>
          <div class="array-grid simple-module-edit-grid">
            ${fields.map(([field, label, kind, listId]) => renderArrayInput(arrayKey, index, field, label, kind, listId, row || {})).join('')}
          </div>
        </section>
      </div>
    </div>
  </div>`;
}

function renderSimpleModuleItemsEditor(profile, arrayKey, rows) {
  if (state.simpleModuleEditArrayKey === arrayKey &&
      !state.simpleModuleEditDraft &&
      (!Number.isInteger(state.simpleModuleEditIndex) || !rows[state.simpleModuleEditIndex])) {
    resetSimpleModuleItemEditor();
  }
  const search = String(state.simpleModuleSearch || '').trim();
  const q = search.toLowerCase();
  const packMode = isPackItemsProfile(profile);
  const vipMode = isVipMembersProfile(profile);
  const vehicleMode = isVehicleRentalProfile(profile);
  const battlepassMode = isBattlepassRewardsProfile(profile);
  const upgradeMode = String(profile && profile.key || '').toLowerCase() === 'item-upgrade';
  let visible = 0;
  const cards = rows.map((row, index) => {
    const title = profile.itemTitle(row, index);
    const summary = profile.itemSummary(row, index);
    const searchText = simpleModuleItemText(row, index, profile);
    const matches = !q || searchText.includes(q);
    if (matches) visible += 1;
    if (packMode) return renderSimplePackItemCard(profile, arrayKey, row, index, matches);
    if (vipMode) return renderSimpleVipMemberCard(profile, arrayKey, row, index, matches);
    if (vehicleMode) return renderSimpleVehicleRentalCard(profile, arrayKey, row, index, matches);
    if (battlepassMode) return renderBattlepassRewardCard(profile, arrayKey, row, index, matches);
    if (upgradeMode) return renderUpgradeRuleCard(profile, arrayKey, row, index, matches);
    const enabled = getNestedValue(row || {}, 'Enabled') !== false && getNestedValue(row || {}, 'enabled') !== false;
    return `<article class="wargm-rule-card simple-module-card simple-rule-row ${state.simpleModuleEditIndex === index && state.simpleModuleEditArrayKey === arrayKey ? 'active' : ''}" data-simple-item-card="true" data-simple-item-array-key="${escapeAttr(arrayKey)}" data-simple-item-search-text="${escapeAttr(searchText)}" ${matches ? '' : 'hidden'}>
      <div class="simple-rule-main">
        <b title="${escapeAttr(title)}">${escapeHtml(title)}</b>
        <span class="wargm-rule-card-status ${enabled ? 'on' : 'off'}">${enabled ? 'включено' : 'выкл'}</span>
      </div>
      <div class="simple-rule-details">
        <span>${escapeHtml(configLabel(arrayKey))}</span>
        <p title="${escapeAttr(summary || title)}">${escapeHtml(summary || title)}</p>
      </div>
      <div class="wargm-rule-card-actions simple-rule-actions">
        <button type="button" class="mini-action primary" data-simple-item-open="${index}" data-simple-item-array-key="${escapeAttr(arrayKey)}">Редактировать</button>
        <button type="button" class="mini-action danger" data-array-remove="${escapeAttr(arrayKey)}" data-array-index="${index}">Удалить</button>
      </div>
    </article>`;
  }).join('');
  return `<div class="field wide array-editor simple-module-items-editor ${packMode ? 'pack-module-items-editor' : ''} ${vipMode ? 'vip-module-items-editor' : ''} ${vehicleMode ? 'vehicle-rental-items-editor' : ''} ${battlepassMode ? 'battlepass-items-editor' : ''} ${upgradeMode ? 'upgrade-items-editor' : ''}" data-simple-array-key="${escapeAttr(arrayKey)}">
    <div class="array-title simple-module-title">
      <label>${escapeHtml(profile.listTitle)}</label>
      <button type="button" class="mini-action primary" data-array-add="${escapeAttr(arrayKey)}">${escapeHtml(profile.addLabel)}</button>
    </div>
    <div class="wargm-rules-toolbar simple-module-toolbar">
      <input class="wargm-rule-search" data-simple-item-search placeholder="${escapeAttr(profile.searchPlaceholder)}" value="${escapeAttr(search)}" />
      <span class="wargm-rule-search-count" data-simple-item-search-count>${visible} / ${rows.length}</span>
    </div>
    <div class="wargm-rule-list simple-module-list ${packMode || vipMode || vehicleMode || battlepassMode || upgradeMode ? 'pack-module-list' : ''} ${vipMode ? 'vip-module-list' : ''} ${vehicleMode ? 'vehicle-rental-list' : ''} ${battlepassMode ? 'battlepass-list' : ''} ${upgradeMode ? 'upgrade-rule-list' : ''}">
      ${cards}
      <div class="muted-line wargm-rule-search-empty" data-simple-item-search-empty ${visible ? 'hidden' : ''}>${escapeHtml(profile.emptyText)}</div>
    </div>
  </div>`;
}

function clearSimpleModuleItemPortal() {
  document.querySelectorAll('[data-simple-item-portal="true"]').forEach(node => node.remove());
}

function renderSimpleModuleItemPortal(config = state.moduleDraft) {
  clearSimpleModuleItemPortal();
  const profile = simpleModuleProfile();
  if (!profile) return;
  const fallbackKey = simpleModuleTabArrayKey(config || {}, profile);
  const arrayKey = state.simpleModuleEditArrayKey || fallbackKey;
  const rows = Array.isArray(config && config[arrayKey]) ? config[arrayKey] : [];
  if (!state.simpleModuleEditDraft && (!Number.isInteger(state.simpleModuleEditIndex) || !rows[state.simpleModuleEditIndex])) return;
  let modalProfile = profile;
  if (arrayKey.toLowerCase() === simpleModuleVipArrayKey(config || {}, profile).toLowerCase()) {
    modalProfile = simpleModuleListProfile(profile, 'vip-items');
  } else {
    const extra = simpleModuleExtraArrays(profile).find(entry => simpleModuleExtraArrayKey(config || {}, entry).toLowerCase() === arrayKey.toLowerCase());
    if (extra) modalProfile = simpleModuleListProfile(profile, extra.tab);
  }
  const row = state.simpleModuleEditDraft || rows[state.simpleModuleEditIndex] || {};
  const index = Number.isInteger(state.simpleModuleEditIndex) ? state.simpleModuleEditIndex : -1;
  const holder = document.createElement('div');
  holder.innerHTML = renderSimpleModuleItemModal(modalProfile, arrayKey, index, row);
  const modal = holder.firstElementChild;
  if (!modal) return;
  modal.dataset.simpleItemPortal = 'true';
  document.body.appendChild(modal);
}

function simpleSettingKind(value, configuredKind) {
  if (configuredKind) return configuredKind;
  if (typeof value === 'boolean') return 'checkbox';
  if (typeof value === 'number') return 'number';
  if (Array.isArray(value)) {
    return value.every(entry => typeof entry === 'number') ? 'number-list' : 'line-list';
  }
  if (value && typeof value === 'object') return 'json';
  return 'text';
}

function simpleSettingSummary(value, kind) {
  if (kind === 'checkbox') return value === true ? 'включено' : 'выключено';
  if (kind === 'chat-lines') {
    const lines = normalizeChatLines(value);
    if (!lines.length) return 'не задано — используется динамическая справка';
    const first = lines[0].length > 54 ? `${lines[0].slice(0, 51)}...` : lines[0];
    return `${formatRussianLineCount(lines.length)} · ${first}`;
  }
  if (kind === 'number-list' || kind === 'line-list') {
    const rows = Array.isArray(value) ? value : [];
    return rows.length ? rows.join(', ') : 'не задано';
  }
  if (kind === 'json') {
    if (!value || typeof value !== 'object') return 'не задано';
    return `${Object.keys(value).length} параметров`;
  }
  if (value === '' || value == null) return 'не задано';
  return String(value);
}

function simpleSettingKindLabel(kind) {
  return {
    checkbox: 'переключатель',
    number: 'число',
    text: 'текст',
    'number-list': 'список',
    'line-list': 'список',
    'chat-lines': 'строки чата',
    json: 'json'
  }[kind] || 'параметр';
}

function simpleSettingMeta(profile, path, value, configured) {
  const field = configured || [];
  return {
    path,
    label: field[1] || configLabel(path.split('.').pop()),
    kind: simpleSettingKind(value, field[2]),
    listId: field[3] || '',
    hint: field[4] || ''
  };
}

function simpleSettingConfiguredFields(profile, config, mode) {
  const vipKey = simpleModuleVipSettingsKey(config || {}, profile);
  if (mode === 'vip') {
    return (profile.vipSettingsFields || []).map(field => [`${vipKey}.${field[0]}`, field[1], field[2], field[3], field[4]]);
  }
  return profile.settingsFields || [];
}

function simpleSettingCardsForProfile(config, profile, mode = 'settings') {
  const explicit = simpleSettingConfiguredFields(profile, config, mode);
  const seen = new Set();
  const rows = [];
  explicit.forEach(field => {
    const path = field[0];
    const value = getNestedValue(config || {}, path);
    seen.add(path.toLowerCase());
    rows.push({ meta: simpleSettingMeta(profile, path, value, field), value });
  });
  if (mode === 'vip') return rows;

  const excluded = new Set([
    simpleModuleArrayKey(config || {}, profile).toLowerCase(),
    simpleModuleVipArrayKey(config || {}, profile).toLowerCase(),
    simpleModuleVipSettingsKey(config || {}, profile).toLowerCase()
  ]);
  simpleModuleExtraArrays(profile).forEach(extra => excluded.add(simpleModuleExtraArrayKey(config || {}, extra).toLowerCase()));
  Object.entries(config || {}).forEach(([key, value]) => {
    const low = key.toLowerCase();
    if (excluded.has(low) || seen.has(low)) return;
    rows.push({ meta: simpleSettingMeta(profile, key, value), value });
  });
  return rows;
}

function renderSimpleSettingCard(row) {
  const meta = row.meta;
  const summary = simpleSettingSummary(row.value, meta.kind);
  const isToggle = meta.kind === 'checkbox';
  const openAttr = isToggle ? '' : ` data-simple-setting-open="${escapeAttr(meta.path)}"`;
  const side = isToggle
    ? `<div class="simple-setting-card-side simple-setting-toggle-side">
        ${renderToggleInput(`data-simple-setting-path="${escapeAttr(meta.path)}" data-simple-setting-kind="checkbox" data-simple-setting-inline="true" aria-label="${escapeAttr(meta.label)}"`, row.value === true)}
      </div>`
    : `<div class="simple-setting-card-side">
        <button type="button" class="mini-action" data-simple-setting-open="${escapeAttr(meta.path)}">Изменить</button>
      </div>`;
  return `<article class="simple-setting-card ${isToggle ? 'is-toggle' : ''}"${openAttr}>
    <div class="simple-setting-card-main">
      <span>${escapeHtml(meta.label)}</span>
      ${meta.hint ? `<small>${escapeHtml(meta.hint)}</small>` : ''}
    </div>
    ${isToggle ? '' : `<strong class="simple-setting-card-value" title="${escapeAttr(summary)}">${escapeHtml(summary)}</strong>`}
    ${side}
  </article>`;
}

function renderSimpleSettingsPanel(config, profile) {
  const cards = simpleSettingCardsForProfile(config, profile, 'settings');
  return `<section class="setting-group card module-basic-settings simple-module-settings simple-settings-click-panel">
    <div class="simple-settings-head">
      <div>
        <h3><span class="icon">⚙</span> Основные настройки</h3>
        <p>Переключатели меняются прямо в строке. Остальные параметры открываются отдельным окном.</p>
      </div>
    </div>
    <div class="simple-setting-card-grid">${cards.length ? cards.map(renderSimpleSettingCard).join('') : '<div class="muted-line">Основных параметров пока нет.</div>'}</div>
  </section>`;
}

function renderSimpleVipSettingsPanel(config, profile) {
  const cards = simpleSettingCardsForProfile(config, profile, 'vip');
  return `<section class="setting-group card simple-vip-settings-panel">
    <div class="simple-settings-head">
      <div>
        <h3>${escapeHtml(profile.vipSettingsTitle || 'Настройки VIP')}</h3>
        <p>${escapeHtml(profile.vipSettingsSubtitle || 'Переключатели меняются прямо в строке, остальные VIP-параметры открываются отдельным окном.')}</p>
      </div>
    </div>
    <div class="simple-setting-card-grid">${cards.map(renderSimpleSettingCard).join('')}</div>
  </section>`;
}

function simpleSettingMetaForPath(config, profile, path) {
  const all = simpleSettingConfiguredFields(profile, config, 'settings').concat(simpleSettingConfiguredFields(profile, config, 'vip'));
  const configured = all.find(field => String(field[0]).toLowerCase() === String(path).toLowerCase());
  const value = getNestedValue(config || {}, path);
  return simpleSettingMeta(profile, path, value, configured);
}

function formatRussianLineCount(value) {
  const count = Math.max(0, Math.floor(Number(value) || 0));
  const lastTwo = count % 100;
  const last = count % 10;
  const word = lastTwo >= 11 && lastTwo <= 14
    ? 'строк'
    : (last === 1 ? 'строка' : (last >= 2 && last <= 4 ? 'строки' : 'строк'));
  return `${count} ${word}`;
}

function normalizeChatLines(value) {
  const raw = Array.isArray(value) ? value : [value];
  const lines = [];
  raw.forEach(entry => {
    String(entry == null ? '' : entry).replace(/\r\n?/g, '\n').split('\n').forEach(line => {
      const clean = String(line || '').trim();
      if (clean) lines.push(clean);
    });
  });
  return lines;
}

function bridgeChatMessageByteLimit() {
  const bridgeModule = (state.modules || []).find(module => String(module && module.key || '').toLowerCase() === 'bridge-safety');
  const bridgeConfig = bridgeModule && bridgeModule.config && typeof bridgeModule.config === 'object' ? bridgeModule.config : {};
  const raw = Number(getNestedValue(bridgeConfig, 'MaxChatMessageBytes') ?? getNestedValue(bridgeConfig, 'maxChatMessageBytes'));
  return Number.isFinite(raw) ? Math.max(16, Math.min(220, Math.floor(raw))) : 220;
}

function isChatResponseModuleKey(key) {
  const normalized = String(key || '').trim().toLowerCase();
  return normalized === 'help-response' || normalized === 'info-response';
}

function chatResponseCommandName() {
  return String(state.selectedModule || '').trim().toLowerCase() === 'info-response' ? '/info' : '/help';
}

function chatLineEditorLimits(config) {
  const rawMaxLines = Number(getNestedValue(config || {}, 'MaxLines'));
  const rawMaxBytes = Number(getNestedValue(config || {}, 'MaxLineBytes'));
  const configuredMaxLineBytes = Number.isFinite(rawMaxBytes) ? Math.max(16, Math.min(220, Math.floor(rawMaxBytes))) : 220;
  const bridgeMaxLineBytes = bridgeChatMessageByteLimit();
  return {
    maxLines: Number.isFinite(rawMaxLines) ? Math.max(1, Math.min(40, Math.floor(rawMaxLines))) : 24,
    maxLineBytes: Math.min(configuredMaxLineBytes, bridgeMaxLineBytes),
    configuredMaxLineBytes,
    bridgeMaxLineBytes
  };
}

function chatLineByteLength(value) {
  const text = String(value || '');
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(text).length;
  return unescape(encodeURIComponent(text)).length;
}

function chatLineCharacterCount(value) {
  return Array.from(String(value || '')).length;
}

function chatLinesForPath(path) {
  return normalizeChatLines(getNestedValue(state.moduleDraft || {}, path));
}

function setChatLinesForPath(path, lines) {
  if (!state.moduleDraft || !path) return [];
  const normalized = normalizeChatLines(lines);
  setNestedValue(state.moduleDraft, path, normalized.join('\n'));
  setModuleDraftText();
  return normalized;
}

function chatLineEditorIssues(lines, limits) {
  const issues = [];
  if (lines.length > limits.maxLines) issues.push(`Максимум ${formatRussianLineCount(limits.maxLines)}.`);
  lines.forEach((line, index) => {
    const bytes = chatLineByteLength(line);
    if (bytes > limits.maxLineBytes) issues.push(`Строка ${index + 1}: ${bytes}/${limits.maxLineBytes} байт.`);
  });
  return issues;
}

function chatResponseInteger(config, path, fallback, min, max, label, issues) {
  const value = Number(getNestedValue(config || {}, path));
  if (!Number.isFinite(value) || Math.floor(value) !== value || value < min || value > max) {
    issues.push(`${label}: укажите целое значение от ${min} до ${max}.`);
    return fallback;
  }
  return value;
}

function ensureChatResponseDefaults(config) {
  if (!config || typeof config !== 'object') return config;
  const defaults = {
    MaxLines: 24,
    MaxLineBytes: bridgeChatMessageByteLimit(),
    LineDelayMs: 140
  };
  Object.entries(defaults).forEach(([path, fallback]) => {
    const value = getNestedValue(config, path);
    if (value == null || String(value).trim() === '') setNestedValue(config, path, fallback);
  });
  return config;
}

function validateChatResponseDraft(config) {
  ensureChatResponseDefaults(config);
  const issues = [];
  const maxLines = chatResponseInteger(config, 'MaxLines', 24, 1, 40, 'Максимум строк', issues);
  const maxLineBytes = chatResponseInteger(config, 'MaxLineBytes', 220, 16, 220, 'Максимум байт на строку', issues);
  chatResponseInteger(config, 'LineDelayMs', 140, 40, 1000, 'Пауза между строками', issues);
  const bridgeMaxLineBytes = bridgeChatMessageByteLimit();
  if (maxLineBytes > bridgeMaxLineBytes) {
    issues.push(`Максимум байт на строку не может быть выше лимита bridge: ${bridgeMaxLineBytes}.`);
  }
  const limits = { maxLines, maxLineBytes: Math.min(maxLineBytes, bridgeMaxLineBytes) };
  [
    ['Text', 'Русская версия'],
    ['EnglishText', 'Английская версия']
  ].forEach(([path, label]) => {
    const lineIssues = chatLineEditorIssues(normalizeChatLines(getNestedValue(config || {}, path)), limits);
    lineIssues.forEach(issue => issues.push(`${label}: ${issue}`));
  });
  if (issues.length) throw new Error(`Исправьте настройки ${chatResponseCommandName()} перед сохранением: ${issues.join(' ')}`);
  return limits;
}

function chatResponseLanguageMeta(path) {
  const english = String(path || '').toLowerCase().includes('english');
  const command = chatResponseCommandName();
  const fallback = command === '/info'
    ? 'Если свой текст выключен или здесь нет строк, игра покажет стандартное динамическое описание команд.'
    : 'Если свой текст выключен или здесь нет строк, игра покажет стандартную динамическую справку.';
  return english
    ? { label: 'Английская версия', fallback: 'Если она пустая, англоязычный игрок увидит русскую версию.' }
    : { label: 'Русская версия', fallback };
}

function renderChatLinesEditor(path, meta, value) {
  const lines = normalizeChatLines(value);
  const limits = chatLineEditorLimits(state.moduleDraft || {});
  const language = chatResponseLanguageMeta(path);
  const issues = chatLineEditorIssues(lines, limits);
  const preview = lines.length
    ? lines.map((line, index) => `<div class="chat-lines-preview-row"><span>${index + 1}</span><p>${escapeHtml(line)}</p></div>`).join('')
    : `<div class="chat-lines-preview-empty">${escapeHtml(language.fallback)}</div>`;
  const rows = lines.length
    ? lines.map((line, index) => {
      const chars = chatLineCharacterCount(line);
      const bytes = chatLineByteLength(line);
      const over = bytes > limits.maxLineBytes;
      return `<div class="chat-lines-editor-row ${over ? 'is-invalid' : ''}" data-chat-lines-row="${index}">
        <span class="chat-lines-editor-index">${index + 1}</span>
        <div class="chat-lines-editor-input-wrap">
          <input type="text" data-chat-lines-input="true" data-chat-lines-index="${index}" value="${escapeAttr(line)}" autocomplete="off" />
          <small data-chat-lines-line-status="${index}" class="${over ? 'is-invalid' : ''}">${chars} симв. · ${bytes}/${limits.maxLineBytes} байт</small>
        </div>
        <div class="chat-lines-editor-actions" aria-label="Порядок строки ${index + 1}">
          <button type="button" class="mini-action" data-chat-lines-action="move-up" data-chat-lines-index="${index}" ${index === 0 ? 'disabled' : ''} title="Поднять строку">↑</button>
          <button type="button" class="mini-action" data-chat-lines-action="move-down" data-chat-lines-index="${index}" ${index >= lines.length - 1 ? 'disabled' : ''} title="Опустить строку">↓</button>
          <button type="button" class="mini-action danger" data-chat-lines-action="remove" data-chat-lines-index="${index}">Удалить</button>
        </div>
      </div>`;
    }).join('')
    : '<div class="chat-lines-editor-empty">Строк пока нет. Добавьте первую ниже или вставьте несколько строк сразу.</div>';
  return `<div class="chat-lines-editor" data-chat-lines-editor="true" data-chat-lines-path="${escapeAttr(path)}">
    <div class="chat-lines-editor-head">
      <div><span>${escapeHtml(language.label)}</span><p>Одна строка = одно сообщение игрового чата. Порядок здесь — порядок отправки.</p></div>
      <b data-chat-lines-summary="true">${formatRussianLineCount(lines.length)} / ${limits.maxLines}</b>
    </div>
    <div class="chat-lines-editor-list">${rows}</div>
    <div class="chat-lines-editor-add">
      <input type="text" data-chat-lines-new="true" placeholder="Новая строка ${escapeAttr(chatResponseCommandName())}" autocomplete="off" />
      <button type="button" class="mini-action primary" data-chat-lines-action="append">Добавить строку</button>
    </div>
    <div class="chat-lines-editor-bulk">
      <label>Вставить несколько строк</label>
      <textarea data-chat-lines-bulk="true" spellcheck="false" placeholder="Каждая новая строка станет отдельным сообщением"></textarea>
      <button type="button" class="mini-action" data-chat-lines-action="append-bulk">Добавить строки</button>
    </div>
    <div class="chat-lines-editor-feedback ${issues.length ? 'is-invalid' : ''}" data-chat-lines-feedback="true">
      ${issues.length ? escapeHtml(issues.join(' ')) : `Лимиты: до ${formatRussianLineCount(limits.maxLines)}, до ${limits.maxLineBytes} байт на строку.`}
    </div>
    <section class="chat-lines-preview">
      <div><h4>Предпросмотр отправки</h4><p>Игра отправит эти строки с заданной паузой. Технические команды не подставляются.</p></div>
      <div data-chat-lines-preview="true">${preview}</div>
    </section>
  </div>`;
}

function updateChatLinesEditorFeedback(editor) {
  if (!editor) return;
  const path = editor.dataset.chatLinesPath || '';
  const lines = chatLinesForPath(path);
  const limits = chatLineEditorLimits(state.moduleDraft || {});
  const issues = chatLineEditorIssues(lines, limits);
  const summary = editor.querySelector('[data-chat-lines-summary]');
  if (summary) summary.textContent = `${formatRussianLineCount(lines.length)} / ${limits.maxLines}`;
  const feedback = editor.querySelector('[data-chat-lines-feedback]');
  if (feedback) {
    feedback.classList.toggle('is-invalid', issues.length > 0);
    feedback.textContent = issues.length ? issues.join(' ') : `Лимиты: до ${formatRussianLineCount(limits.maxLines)}, до ${limits.maxLineBytes} байт на строку.`;
  }
  const preview = editor.querySelector('[data-chat-lines-preview]');
  if (preview) {
    const language = chatResponseLanguageMeta(path);
    preview.innerHTML = lines.length
      ? lines.map((line, index) => `<div class="chat-lines-preview-row"><span>${index + 1}</span><p>${escapeHtml(line)}</p></div>`).join('')
      : `<div class="chat-lines-preview-empty">${escapeHtml(language.fallback)}</div>`;
  }
}

function syncChatLinesEditorInput(target) {
  const editor = target && target.closest && target.closest('[data-chat-lines-editor="true"]');
  if (!editor || !state.moduleDraft) return false;
  const index = Number(target.dataset.chatLinesIndex);
  const path = editor.dataset.chatLinesPath || '';
  const lines = chatLinesForPath(path);
  if (!Number.isInteger(index) || index < 0 || index >= lines.length) return false;
  lines[index] = String(target.value || '').trim();
  const normalized = setChatLinesForPath(path, lines);
  if (normalized.length !== lines.length) {
    renderSimpleSettingPortal(state.moduleDraft);
    return true;
  }
  const bytes = chatLineByteLength(normalized[index]);
  const chars = chatLineCharacterCount(normalized[index]);
  const limit = chatLineEditorLimits(state.moduleDraft).maxLineBytes;
  const over = bytes > limit;
  const row = target.closest('[data-chat-lines-row]');
  if (row) row.classList.toggle('is-invalid', over);
  const status = row && row.querySelector('[data-chat-lines-line-status]');
  if (status) {
    status.classList.toggle('is-invalid', over);
    status.textContent = `${chars} симв. · ${bytes}/${limit} байт`;
  }
  updateChatLinesEditorFeedback(editor);
  return true;
}

function appendChatLinesForEditor(editor, incoming, replaceIndex = null) {
  if (!editor || !state.moduleDraft) return { accepted: 0, rejected: 0 };
  const path = editor.dataset.chatLinesPath || '';
  const limits = chatLineEditorLimits(state.moduleDraft);
  const candidates = normalizeChatLines(incoming);
  if (!candidates.length) return { accepted: 0, rejected: 0 };
  const lines = chatLinesForPath(path);
  if (Number.isInteger(replaceIndex) && replaceIndex >= 0 && replaceIndex < lines.length) lines.splice(replaceIndex, 1);
  const available = Math.max(0, limits.maxLines - lines.length);
  const accepted = candidates.slice(0, available);
  lines.push(...accepted);
  setChatLinesForPath(path, lines);
  return { accepted: accepted.length, rejected: Math.max(0, candidates.length - accepted.length) };
}

function handleChatLinesEditorClick(evt, modal) {
  const action = evt.target && evt.target.closest && evt.target.closest('[data-chat-lines-action]');
  if (!action) return false;
  const editor = action.closest('[data-chat-lines-editor="true"]');
  if (!editor) return false;
  evt.preventDefault();
  evt.stopPropagation();
  const path = editor.dataset.chatLinesPath || '';
  const kind = action.dataset.chatLinesAction || '';
  const index = Number(action.dataset.chatLinesIndex);
  if (kind === 'append') {
    const input = editor.querySelector('[data-chat-lines-new]');
    const result = appendChatLinesForEditor(editor, input ? input.value : '');
    if (!result.accepted) toast('Введите непустую строку перед добавлением.');
    else if (result.rejected) toast(`Добавлено ${formatRussianLineCount(result.accepted)}; лимит ${formatRussianLineCount(chatLineEditorLimits(state.moduleDraft).maxLines)}.`);
  } else if (kind === 'append-bulk') {
    const input = editor.querySelector('[data-chat-lines-bulk]');
    const result = appendChatLinesForEditor(editor, input ? input.value : '');
    if (!result.accepted) toast('Вставьте хотя бы одну непустую строку.');
    else if (result.rejected) toast(`Добавлено ${formatRussianLineCount(result.accepted)}; остальные строки не поместились в лимит.`);
  } else {
    const lines = chatLinesForPath(path);
    if (!Number.isInteger(index) || index < 0 || index >= lines.length) return true;
    if (kind === 'remove') lines.splice(index, 1);
    else if (kind === 'move-up' && index > 0) [lines[index - 1], lines[index]] = [lines[index], lines[index - 1]];
    else if (kind === 'move-down' && index < lines.length - 1) [lines[index + 1], lines[index]] = [lines[index], lines[index + 1]];
    setChatLinesForPath(path, lines);
  }
  renderSimpleSettingPortal(state.moduleDraft);
  return true;
}

function renderSimpleSettingControl(path, meta, value) {
  const attrs = `data-simple-setting-path="${escapeAttr(path)}" data-simple-setting-kind="${escapeAttr(meta.kind)}"`;
  if (meta.kind === 'checkbox') return `<div class="setting-control">${renderToggleInput(attrs, value === true)}</div>`;
  if (meta.kind === 'number') {
    const selectedKey = String(state.selectedModule || '').toLowerCase();
    const numericBounds = isChatResponseModuleKey(selectedKey) && path === 'LineDelayMs'
      ? ' min="40" max="1000" step="1"'
      : (isChatResponseModuleKey(selectedKey) && path === 'MaxLines'
        ? ' min="1" max="40" step="1"'
        : (isChatResponseModuleKey(selectedKey) && path === 'MaxLineBytes'
          ? ` min="16" max="${bridgeChatMessageByteLimit()}" step="1"`
          : ''));
    const effectiveValue = isChatResponseModuleKey(selectedKey) && path === 'MaxLineBytes' &&
      (value == null || String(value).trim() === '')
      ? bridgeChatMessageByteLimit()
      : value;
    return `<input ${attrs} type="number"${numericBounds} value="${escapeAttr(effectiveValue || 0)}" />`;
  }
  if (String(meta.kind || '').startsWith('select:')) {
    const options = String(meta.kind).slice(7).split('|')
      .map(opt => `<option value="${escapeAttr(opt)}" ${String(value) === opt ? 'selected' : ''}>${escapeHtml(selectOptionLabel(opt))}</option>`)
      .join('');
    return `<select ${attrs}>${options}</select>`;
  }
  if (meta.kind === 'number-list' || meta.kind === 'line-list') {
    const text = Array.isArray(value) ? value.join(meta.kind === 'number-list' ? ', ' : '\n') : String(value || '');
    return `<textarea ${attrs} spellcheck="false">${escapeHtml(text)}</textarea>`;
  }
  if (meta.kind === 'chat-lines') return renderChatLinesEditor(path, meta, value);
  if (meta.kind === 'json') {
    const text = value && typeof value === 'object' ? JSON.stringify(value, null, 2) : '{}';
    return `<textarea ${attrs} data-simple-setting-json="true" spellcheck="false">${escapeHtml(text)}</textarea>`;
  }
  const listAttr = meta.listId ? ` list="${escapeAttr(meta.listId)}"` : '';
  return `<input ${attrs}${listAttr} value="${escapeAttr(value || '')}" />`;
}

function renderSimpleSettingModal(config, profile, path) {
  const meta = simpleSettingMetaForPath(config || {}, profile, path);
  const value = getNestedValue(config || {}, path);
  return `<div class="wargm-rule-modal simple-module-modal simple-setting-modal" data-simple-setting-overlay="true" role="dialog" aria-modal="true">
    <div class="wargm-rule-modal-card simple-module-modal-card simple-setting-modal-card">
      <div class="wargm-rule-modal-head">
        <div>
          <span class="wargm-modal-kicker">Настройка параметра</span>
          <h3>${escapeHtml(meta.label)}</h3>
          <p>${escapeHtml(path)}</p>
        </div>
        <div class="wargm-rule-modal-actions">
          <button type="button" class="mini-action primary" data-simple-setting-save="true">Сохранить настройку</button>
          <button type="button" class="mini-action" data-simple-setting-close="true">Закрыть</button>
        </div>
      </div>
      <div class="wargm-rule-layout simple-module-modal-body">
        <section class="wargm-rule-section">
          <div class="wargm-rule-section-title">
            <div><h4>${escapeHtml(meta.label)}</h4><p>После сохранения параметра общий конфиг модуля тоже будет сохранён.</p></div>
          </div>
          <div class="simple-setting-modal-field">${renderSimpleSettingControl(path, meta, value)}</div>
        </section>
      </div>
    </div>
  </div>`;
}

function clearSimpleSettingPortal() {
  document.querySelectorAll('[data-simple-setting-portal="true"]').forEach(node => node.remove());
}

function renderSimpleSettingPortal(config = state.moduleDraft) {
  clearSimpleSettingPortal();
  const profile = simpleModuleProfile();
  if (!profile || !state.simpleSettingEditPath) return;
  const holder = document.createElement('div');
  holder.innerHTML = renderSimpleSettingModal(config || {}, profile, state.simpleSettingEditPath);
  const modal = holder.firstElementChild;
  if (!modal) return;
  modal.dataset.simpleSettingPortal = 'true';
  document.body.appendChild(modal);
}

function renderSimpleModuleFields(config, profile) {
  const showMainItems = profile.noItems !== true;
  const arrayKey = simpleModuleArrayKey(config || {}, profile);
  const rows = showMainItems ? simpleModuleRows(config || {}, arrayKey) : [];
  const vipArrayKey = simpleModuleVipArrayKey(config || {}, profile);
  const vipRows = simpleModuleHasVipItems(profile) ? simpleModuleRows(config || {}, vipArrayKey) : [];
  const active = simpleModuleTab(profile);
  const settings = renderSimpleSettingsPanel(config || {}, profile);
  const mainItemsProfile = showMainItems ? simpleModuleListProfile(profile, 'items') : null;
  const vipItemsProfile = simpleModuleListProfile(profile, 'vip-items');
  const extraPanels = simpleModuleExtraArrays(profile).map(extra => {
    const extraKey = simpleModuleExtraArrayKey(config || {}, extra);
    const extraRows = simpleModuleRows(config || {}, extraKey);
    const extraProfile = simpleModuleListProfile(profile, extra.tab);
    return {
      tab: extra.tab,
      label: extra.itemsTab || extra.tab,
      rows: extraRows,
      html: renderSimpleModuleItemsEditor(extraProfile, extraKey, extraRows)
    };
  });
  const vipPanel = simpleModuleHasVipItems(profile)
    ? `<div class="simple-vip-products-stack">
        ${renderSimpleModuleItemsEditor(vipItemsProfile, vipArrayKey, vipRows)}
        ${renderSimpleVipSettingsPanel(config || {}, profile)}
      </div>`
    : '';
  return `<div class="module-settings-view wargm-module-view simple-module-view">
    <div class="wargm-module-tabs simple-module-tabs" role="tablist" aria-label="${escapeAttr(profile.listTitle)}">
      <button type="button" class="tab wargm-module-tab ${active === 'settings' ? 'active' : ''}" data-simple-config-tab="settings" role="tab" aria-selected="${active === 'settings'}">Основные настройки</button>
      ${showMainItems ? `<button type="button" class="tab wargm-module-tab ${active === 'items' ? 'active' : ''}" data-simple-config-tab="items" role="tab" aria-selected="${active === 'items'}">${escapeHtml(profile.itemsTab)} <span>${rows.length}</span></button>` : ''}
      ${simpleModuleHasVipItems(profile) ? `<button type="button" class="tab wargm-module-tab ${active === 'vip-items' ? 'active' : ''}" data-simple-config-tab="vip-items" role="tab" aria-selected="${active === 'vip-items'}">${escapeHtml(profile.vipItemsTab || 'VIP товары')} <span>${vipRows.length}</span></button>` : ''}
      ${extraPanels.map(panel => `<button type="button" class="tab wargm-module-tab ${active === panel.tab ? 'active' : ''}" data-simple-config-tab="${escapeAttr(panel.tab)}" role="tab" aria-selected="${active === panel.tab}">${escapeHtml(panel.label)} <span>${panel.rows.length}</span></button>`).join('')}
    </div>
    <div class="wargm-module-tab-panel" data-simple-config-panel="settings" ${active === 'settings' ? '' : 'hidden'}>${settings}</div>
    ${showMainItems ? `<div class="wargm-module-tab-panel simple-products-panel" data-simple-config-panel="items" ${active === 'items' ? '' : 'hidden'}>${renderSimpleModuleItemsEditor(mainItemsProfile, arrayKey, rows)}</div>` : ''}
    ${simpleModuleHasVipItems(profile) ? `<div class="wargm-module-tab-panel simple-products-panel simple-vip-products-panel" data-simple-config-panel="vip-items" ${active === 'vip-items' ? '' : 'hidden'}>${vipPanel}</div>` : ''}
    ${extraPanels.map(panel => `<div class="wargm-module-tab-panel simple-products-panel" data-simple-config-panel="${escapeAttr(panel.tab)}" ${active === panel.tab ? '' : 'hidden'}>${panel.html}</div>`).join('')}
  </div>`;
}

function worldEditPresets(config = {}) {
  const configured = Array.isArray(config.Presets) ? config.Presets : (Array.isArray(config.presets) ? config.presets : []);
  const fallback = [
    { Name: 'Military crate', Mesh: '/Game/ConZ_Files/Models/Objects/Outdoor/Crate/SM_Crate_03_A_Military.SM_Crate_03_A_Military' },
    { Name: 'Stone wall long', Mesh: '/Game/ConZ_Files/Models/Objects/Outdoor/SM_StoneWall_02/SM_Stone_Wall_02_Long.SM_Stone_Wall_02_Long' },
    { Name: 'Cardboard box', Mesh: '/Game/ConZ_Files/Models/Objects/Indoor/Storage/Cardboard_Boxes/SM_Cardboard_Box_05_Empty.SM_Cardboard_Box_05_Empty' },
    { Name: 'Door frame', Mesh: '/Game/ConZ_Files/Models/Buildings/Stone_House/Doors-Windows/SM_Door_Int_01_Frame.SM_Door_Int_01_Frame' },
    { Name: 'Street lamp', Mesh: '/Game/ConZ_Files/Models/Objects/Outdoor/Street_Lamp/SM_Street_lamp_02.SM_Street_Lamp_02' }
  ];
  return (configured.length ? configured : fallback)
    .map(item => ({
      name: item.Name || item.name || item.Label || item.label || item.Mesh || item.mesh || '',
      mesh: item.Mesh || item.mesh || item.SourceMeshFullName || item.sourceMeshFullName || ''
    }))
    .filter(item => item.mesh);
}

function worldEditServerOnlyEntities(config = {}) {
  const configured = Array.isArray(config.ServerOnlyEntities) ? config.ServerOnlyEntities : (Array.isArray(config.serverOnlyEntities) ? config.serverOnlyEntities : []);
  const fallback = [
    { Name: 'Refrigerator', Entity: 'Refrigerator_ES' },
    { Name: 'Refrigerator unusable', Entity: 'Refrigerator_Unusable_ES' },
    { Name: 'Kitchen stove', Entity: 'KitchenStove_ES' },
    { Name: 'Kitchen stove unusable', Entity: 'KitchenStove_Unusable_ES' },
    { Name: 'Drill press', Entity: 'Work_Drillpress_02_ES' }
  ];
  return (configured.length ? configured : fallback)
    .map(item => ({
      name: item.Name || item.name || item.Label || item.label || item.Entity || item.entity || '',
      entity: item.Entity || item.entity || item.EntityClass || item.entityClass || ''
    }))
    .filter(item => item.entity);
}

function worldEditDraftValue(config, key, fallback = '') {
  if (!config || typeof config !== 'object') return fallback;
  const lower = key.charAt(0).toLowerCase() + key.slice(1);
  return config[key] != null ? config[key] : (config[lower] != null ? config[lower] : fallback);
}

function renderWorldEditModuleFields(config = {}) {
  const presets = worldEditPresets(config);
  const sourceMesh = worldEditDraftValue(config, 'SourceMeshFullName', presets[0] ? presets[0].mesh : '');
  const selectedIndex = Math.max(0, presets.findIndex(item => item.mesh === sourceMesh));
  const result = state.worldEditLastResult ? formatPanelResult(state.worldEditLastResult) : '';
  return `<div class="module-settings-view world-edit-view">
    <section class="simple-settings-panel">
      <div class="simple-setting-card-grid">
        <article class="simple-setting-card">
          <span>Preset</span>
          <select data-world-edit-field="PresetIndex">
            ${presets.map((item, index) => `<option value="${index}" ${index === selectedIndex ? 'selected' : ''}>${escapeHtml(item.name)}</option>`).join('')}
          </select>
        </article>
        <article class="simple-setting-card simple-setting-card-wide">
          <span>sourceMeshFullName</span>
          <input data-world-edit-field="SourceMeshFullName" value="${escapeAttr(sourceMesh)}" spellcheck="false" />
        </article>
        <article class="simple-setting-card">
          <span>X</span>
          <input data-world-edit-field="X" type="number" step="1" value="${escapeAttr(worldEditDraftValue(config, 'X', 0))}" />
        </article>
        <article class="simple-setting-card">
          <span>Y</span>
          <input data-world-edit-field="Y" type="number" step="1" value="${escapeAttr(worldEditDraftValue(config, 'Y', 0))}" />
        </article>
        <article class="simple-setting-card">
          <span>Z</span>
          <input data-world-edit-field="Z" type="number" step="1" value="${escapeAttr(worldEditDraftValue(config, 'Z', 500))}" />
        </article>
        <article class="simple-setting-card">
          <span>Yaw</span>
          <input data-world-edit-field="Yaw" type="number" step="1" value="${escapeAttr(worldEditDraftValue(config, 'Yaw', 0))}" />
        </article>
        <article class="simple-setting-card">
          <span>Pitch</span>
          <input data-world-edit-field="Pitch" type="number" step="1" value="${escapeAttr(worldEditDraftValue(config, 'Pitch', 0))}" />
        </article>
        <article class="simple-setting-card">
          <span>Roll</span>
          <input data-world-edit-field="Roll" type="number" step="1" value="${escapeAttr(worldEditDraftValue(config, 'Roll', 0))}" />
        </article>
        <article class="simple-setting-card simple-setting-card-wide">
          <span>Label</span>
          <input data-world-edit-field="Label" value="${escapeAttr(worldEditDraftValue(config, 'Label', 'world-edit-test'))}" />
        </article>
      </div>
      <div class="module-actions inline-actions">
        <button class="btn" type="button" data-world-edit-action="player-pos">Координаты игрока</button>
        <button class="btn primary" type="button" data-world-edit-action="spawn">Создать visual</button>
        <button class="btn" type="button" data-world-edit-action="test-crate">Тест crate</button>
        <button class="btn" type="button" data-world-edit-action="state">Проверить journal</button>
      </div>
      <pre class="result world-edit-result" data-world-edit-result>${escapeHtml(result)}</pre>
    </section>
    <section class="simple-settings-panel">
      <div class="module-actions inline-actions">
        <button class="btn" type="button" data-world-edit-action="server-probe">Probe CDO</button>
      </div>
    </section>
  </div>`;
}

function syncWorldEditField(target) {
  if (!target || !target.dataset || !target.dataset.worldEditField) return false;
  if (!state.moduleDraft || typeof state.moduleDraft !== 'object') state.moduleDraft = {};
  const key = target.dataset.worldEditField;
  if (key === 'PresetIndex') {
    const preset = worldEditPresets(state.moduleDraft)[Number(target.value || 0)];
    if (preset) state.moduleDraft.SourceMeshFullName = preset.mesh;
  } else if (['X', 'Y', 'Z', 'Pitch', 'Yaw', 'Roll'].includes(key)) {
    state.moduleDraft[key] = Number(target.value || 0);
  } else {
    state.moduleDraft[key] = target.value;
  }
  setModuleDraftText();
  return true;
}

function collectWorldEditPayload(root = els.moduleFields, forceCrate = false) {
  const pick = field => root && root.querySelector(`[data-world-edit-field="${field}"]`);
  const config = state.moduleDraft || {};
  const crateMesh = '/Game/ConZ_Files/Models/Objects/Outdoor/Crate/SM_Crate_03_A_Military.SM_Crate_03_A_Military';
  const mesh = forceCrate ? crateMesh : String((pick('SourceMeshFullName') && pick('SourceMeshFullName').value) || worldEditDraftValue(config, 'SourceMeshFullName', crateMesh)).trim();
  const numberValue = (field, fallback) => Number((pick(field) && pick(field).value) || worldEditDraftValue(config, field, fallback) || 0);
  return {
    sourceMeshFullName: mesh,
    x: numberValue('X', 0),
    y: numberValue('Y', 0),
    z: numberValue('Z', 500),
    pitch: numberValue('Pitch', 0),
    yaw: numberValue('Yaw', 0),
    roll: numberValue('Roll', 0),
    label: String((pick('Label') && pick('Label').value) || worldEditDraftValue(config, 'Label', forceCrate ? 'world-edit-test-crate' : 'world-edit-visual')).trim()
  };
}

function worldEditPlayerCandidate() {
  const players = Array.isArray(state.players) ? state.players : [];
  const selected = state.selectedPlayerTarget || null;
  if (selected) {
    const matched = findPlayerByIdentity(selected.steamId || '', selected.name || '', selected.runtimeKey || '');
    if (matched && playerHasMapPosition(matched)) return matched;
    if (playerHasMapPosition(selected)) return selected;
  }
  return players.find(playerHasMapPosition) || null;
}

function worldEditLivePlayerCandidate() {
  const players = Array.isArray(state.players) ? state.players : [];
  const selected = state.selectedPlayerTarget || null;
  if (selected) {
    const matched = findPlayerByIdentity(selected.steamId || selected.steam || '', selected.name || '', selected.runtimeKey || '');
    if (matched) return matched;
    return selected;
  }
  return players[0] || null;
}

async function applyWorldEditPlayerPosition() {
  const playersAgeMs = Date.now() - Number(state.playersLastRefreshedAt || 0);
  if (!Array.isArray(state.players) || !state.players.length || playersAgeMs > 10000) {
    await refreshPlayers().catch(() => {});
  }
  const player = worldEditPlayerCandidate();
  if (!player) {
    toast('Нет онлайн-игрока с live-координатами.');
    return false;
  }
  if (!state.moduleDraft || typeof state.moduleDraft !== 'object') state.moduleDraft = {};
  const coords = playerCoords(player);
  const name = pick(player, ['name', 'Name', 'playerName'], 'игрок');
  state.moduleDraft.X = Math.round(coords.x);
  state.moduleDraft.Y = Math.round(coords.y);
  state.moduleDraft.Z = Math.round(coords.z + 120);
  setModuleDraftText();
  state.worldEditLastResult = {
    ok: true,
    message: `Координаты взяты от ${name}: X ${state.moduleDraft.X}, Y ${state.moduleDraft.Y}, Z ${state.moduleDraft.Z}.`,
    data: {
      player: name,
      x: state.moduleDraft.X,
      y: state.moduleDraft.Y,
      z: state.moduleDraft.Z,
      note: 'Z поднят на 120 см для первого визуального теста.'
    }
  };
  renderModuleFields(state.moduleDraft);
  return true;
}

async function runWorldEditAction(action) {
  const resultEl = els.moduleFields && els.moduleFields.querySelector('[data-world-edit-result]');
  if (action === 'player-pos') {
    await applyWorldEditPlayerPosition();
    return;
  }
  if (action === 'state') {
    await showActionResult(resultEl || els.quickResult, async () => {
      const result = await api('/api/plugin-command', {
        method: 'POST',
        body: JSON.stringify({ command: 'editor_actor_overlay_state', args: {}, timeoutMs: 8000 })
      });
      state.worldEditLastResult = result;
      return result;
    });
    return;
  }
  if (action === 'server-probe') {
    await showActionResult(resultEl || els.quickResult, async () => {
      const result = await api('/api/plugin-command', {
        method: 'POST',
        body: JSON.stringify({ command: 'world_edit_server_only_probe', args: {}, timeoutMs: 30000 })
      });
      state.worldEditLastResult = result;
      return result;
    });
    return;
  }
  const payload = collectWorldEditPayload(els.moduleFields, action === 'test-crate');
  if (!payload.sourceMeshFullName) {
    toast('Укажи sourceMeshFullName.');
    return;
  }
  await showActionResult(resultEl || els.quickResult, async () => {
    const result = await api('/api/plugin-command', {
      method: 'POST',
      body: JSON.stringify({ command: 'world_edit_visual_spawn', args: payload, timeoutMs: 8000 })
    });
    state.worldEditLastResult = result;
    return result;
  });
}

function renderModuleFields(config) {
  const entries = Object.entries(config || {});
  if (String(state.selectedModule || '').toLowerCase() === 'world-edit') {
    clearSchedulerJobPortal();
    clearSimpleModuleItemPortal();
    clearSimpleSettingPortal();
    els.moduleFields.innerHTML = renderWorldEditModuleFields(config || {});
    updateModuleCatalogVisibility();
    return;
  }
  if (String(state.selectedModule || '').toLowerCase() === 'scheduled-events') {
    clearSimpleModuleItemPortal();
    clearSimpleSettingPortal();
    els.moduleFields.innerHTML = renderSchedulerModuleFields(config || {});
    updateModuleCatalogVisibility();
    renderSchedulerJobPortal();
    return;
  }
  if (isExternalShopModule()) {
    clearSchedulerJobPortal();
    clearSimpleModuleItemPortal();
    clearSimpleSettingPortal();
    els.moduleFields.innerHTML = renderWargmModuleFields(config || {});
    updateModuleCatalogVisibility();
    return;
  }
  const simpleProfile = simpleModuleProfile();
  if (simpleProfile) {
    clearSchedulerJobPortal();
    els.moduleFields.innerHTML = renderSimpleModuleFields(config || {}, simpleProfile);
    updateModuleCatalogVisibility();
    renderSimpleModuleItemPortal(config || {});
    renderSimpleSettingPortal(config || {});
    return;
  }
  clearSchedulerJobPortal();
  clearSimpleModuleItemPortal();
  clearSimpleSettingPortal();
  els.moduleFields.innerHTML = renderGenericModuleFields(config || {});
  updateModuleCatalogVisibility();
}

function updateWargmRuleSearchFilter() {
  const input = els.moduleFields && els.moduleFields.querySelector('[data-wargm-rule-search]');
  if (!input) return;
  const query = String(input.value || '').trim();
  const q = query.toLowerCase();
  state.wargmRuleSearch = query;
  let visible = 0;
  let total = 0;
  els.moduleFields.querySelectorAll('[data-wargm-rule-card]').forEach(card => {
    total += 1;
    const haystack = String(card.dataset.wargmRuleSearchText || '');
    const match = !q || haystack.includes(q);
    card.hidden = !match;
    if (match) visible += 1;
  });
  const count = els.moduleFields.querySelector('[data-wargm-rule-search-count]');
  if (count) count.textContent = `${visible} / ${total}`;
  const empty = els.moduleFields.querySelector('[data-wargm-rule-search-empty]');
  if (empty) empty.hidden = visible > 0;
}

function updateSimpleModuleSearchFilter() {
  const input = els.moduleFields && (els.moduleFields.querySelector('[data-simple-config-panel]:not([hidden]) [data-simple-item-search]') || els.moduleFields.querySelector('[data-simple-item-search]'));
  if (!input) return;
  const query = String(input.value || '').trim();
  const q = query.toLowerCase();
  state.simpleModuleSearch = query;
  let visible = 0;
  let total = 0;
  const scope = input.closest('[data-simple-config-panel]') || els.moduleFields;
  scope.querySelectorAll('[data-simple-item-card]').forEach(card => {
    total += 1;
    const haystack = String(card.dataset.simpleItemSearchText || '');
    const match = !q || haystack.includes(q);
    card.hidden = !match;
    if (match) visible += 1;
  });
  const count = scope.querySelector('[data-simple-item-search-count]');
  if (count) count.textContent = `${visible} / ${total}`;
  const empty = scope.querySelector('[data-simple-item-search-empty]');
  if (empty) empty.hidden = visible > 0;
}

function syncSimpleSettingInput(target, strictJson = false) {
  if (!target || !target.dataset || !target.dataset.simpleSettingPath || !state.moduleDraft) return false;
  const path = target.dataset.simpleSettingPath;
  const kind = target.dataset.simpleSettingKind || '';
  let value;
  if (kind === 'checkbox' || target.type === 'checkbox') {
    value = target.checked;
  } else if (kind === 'number' || target.type === 'number') {
    value = Number(target.value || 0);
  } else if (kind === 'number-list') {
    value = String(target.value || '')
      .split(/\r?\n|,/)
      .map(x => Number(String(x).trim()))
      .filter(Number.isFinite);
  } else if (kind === 'line-list') {
    value = String(target.value || '')
      .split(/\r?\n|,/)
      .map(x => x.trim())
      .filter(Boolean);
  } else if (kind === 'json') {
    if (!strictJson) return true;
    value = target.value.trim() ? JSON.parse(target.value) : {};
  } else {
    value = target.value;
  }
  setNestedValue(state.moduleDraft, path, value);
  setModuleDraftText();
  return true;
}

function arrayTargetRow(target, key, index) {
  const simplePortal = target && target.closest && target.closest('[data-simple-item-portal="true"]');
  if (simplePortal && state.simpleModuleEditDraft && String(key || '').toLowerCase() === String(state.simpleModuleEditArrayKey || '').toLowerCase()) {
    return { row: state.simpleModuleEditDraft, draft: true };
  }
  if (state.moduleDraft && Array.isArray(state.moduleDraft[key]) && state.moduleDraft[key][index]) {
    return { row: state.moduleDraft[key][index], draft: false };
  }
  return { row: null, draft: false };
}

function markArrayTargetChanged(ctx) {
  if (!ctx || ctx.draft) return;
  setModuleDraftText();
}


function renderModuleCatalog() {
  if (!els.moduleCatalogGrid) return;
  const mode = els.moduleCatalogMode ? els.moduleCatalogMode.value : state.catalogMode;
  const nextMode = mode === 'vehicles' ? 'vehicles' : 'items';
  if (state.catalogMode !== nextMode) state.catalogCategory = '';
  state.catalogMode = nextMode;
  const q = (els.moduleCatalogSearch ? els.moduleCatalogSearch.value : state.catalogSearch || '').trim().toLowerCase();
  state.catalogSearch = q;
  const kind = state.catalogMode === 'vehicles' ? 'vehicle' : 'item';
  const source = kind === 'vehicle' ? state.vehicles : state.items;
  const categories = Array.from(new Set((source || []).map(entry => entry.category || 'Другое').filter(Boolean))).sort((a, b) => a.localeCompare(b));
  if (els.moduleCatalogCategories) {
    els.moduleCatalogCategories.innerHTML = [`<button type="button" class="${state.catalogCategory ? '' : 'active'}" data-catalog-category="">Все</button>`]
      .concat(categories.slice(0, 26).map(category => `<button type="button" class="${state.catalogCategory === category ? 'active' : ''}" data-catalog-category="${escapeAttr(category)}">${escapeHtml(category)}</button>`))
      .join('');
  }
  const filtered = (source || []).filter(entry => {
    if (state.catalogCategory && entry.category !== state.catalogCategory) return false;
    if (!q) return true;
    return [catalogValue(entry, kind), entry.name, entry.displayName, entry.category, entry.runtimeId]
      .some(value => String(value || '').toLowerCase().includes(q));
  }).slice(0, 120);
  els.moduleCatalogGrid.innerHTML = filtered.length
    ? filtered.map(entry => catalogTile(entry, kind)).join('')
    : '<div class="muted-line">Ничего не найдено.</div>';
  if (els.moduleCatalogCount) {
    els.moduleCatalogCount.textContent = `${filtered.length} / ${(source || []).length}`;
  }
  if (els.moduleCatalogSelected) {
    const selected = state.catalogSelected;
    els.moduleCatalogSelected.innerHTML = selected
      ? `<b>${escapeHtml(selected.name || selected.value)}</b><span>${escapeHtml(selected.value || '')}</span><small>${escapeHtml(selected.category || selected.kind || '')}</small>`
      : 'Ничего не выбрано.';
  }
}

function syncModuleField(target) {
  if (syncSimpleSettingInput(target, false)) return;

  if (target.dataset && target.dataset.battlepassItemField) {
    const key = target.dataset.arrayKey;
    const index = Number(target.dataset.arrayIndex);
    const itemIndex = Number(target.dataset.battlepassItemIndex);
    const field = target.dataset.battlepassItemField;
    const ctx = arrayTargetRow(target, key, index);
    if (!ctx.row) return;
    const reward = ctx.row;
    const items = battlepassRewardItems(reward);
    if (!items[itemIndex]) items[itemIndex] = { itemId: 'Apple_2', quantity: 1 };
    if (field === 'ItemId') {
      items[itemIndex].itemId = target.value;
      const rowEl = target.closest && target.closest('.battlepass-item-row');
      const thumb = rowEl && rowEl.querySelector('[data-battlepass-item-thumb]');
      if (thumb) thumb.outerHTML = catalogThumbHtml(target.value, 'item', { className: 'battlepass-item-thumb', attrs: 'data-battlepass-item-thumb="true"', allowGenerated: true });
    } else if (field === 'Quantity') {
      items[itemIndex].quantity = Math.max(1, Math.floor(Number(target.value || 1) || 1));
    }
    setBattlepassRewardItems(reward, items);
    markArrayTargetChanged(ctx);
    return;
  }

  if (target.dataset.ruleItemField) {
    const key = target.dataset.arrayKey;
    const ruleIndex = Number(target.dataset.arrayIndex);
    const itemIndex = Number(target.dataset.ruleItemIndex);
    const field = target.dataset.ruleItemField;
    if (!state.moduleDraft || !Array.isArray(state.moduleDraft[key]) || !state.moduleDraft[key][ruleIndex]) return;
    const rule = state.moduleDraft[key][ruleIndex];
    const itemKey = Array.isArray(rule.items) && !Array.isArray(rule.Items) ? 'items' : 'Items';
    const fieldKey = itemKey === 'items' ? (field === 'ItemId' ? 'itemId' : 'quantity') : field;
    if (!Array.isArray(rule[itemKey])) rule[itemKey] = [];
    if (!rule[itemKey][itemIndex]) rule[itemKey][itemIndex] = itemKey === 'items' ? { itemId: '', quantity: 1 } : { ItemId: '', Quantity: 1 };
    rule[itemKey][itemIndex][fieldKey] = target.type === 'number' ? Number(target.value || 0) : target.value;
    setModuleDraftText();
    if (field === 'ItemId') {
      const rowEl = target.closest && target.closest('.rule-item-row');
      const thumb = rowEl && rowEl.querySelector('[data-rule-item-thumb]');
      if (thumb) thumb.outerHTML = catalogThumbHtml(target.value, 'item', { className: 'rule-item-thumb', attrs: 'data-rule-item-thumb="true"', allowGenerated: true });
    }
    return;
  }

  if (target.dataset.arrayKey) {
    const key = target.dataset.arrayKey;
    const index = Number(target.dataset.arrayIndex);
    const field = target.dataset.arrayField;
    const ctx = arrayTargetRow(target, key, index);
    if (!ctx.row) return;
    const kind = target.dataset.arrayKind || '';
    let value = target.type === 'checkbox' ? target.checked : target.value;
    if (target.type === 'number') value = Number(target.value || 0);
    else if (kind === 'number-list') {
      value = String(target.value || '')
        .split(/\r?\n|,/)
        .map(x => Number(String(x).trim()))
        .filter(Number.isFinite);
    } else if (kind === 'line-list') {
      value = String(target.value || '')
        .split(/\r?\n|,/)
        .map(x => x.trim())
        .filter(Boolean);
    }
    setNestedValue(ctx.row, field, value);
    if (field === 'DeliveryMode') {
      if (value === 'Fame') ctx.row.UiDeliveryMode = 'Fame';
      else delete ctx.row.UiDeliveryMode;
    }
    markArrayTargetChanged(ctx);
    return;
  }

  if (target.dataset.objectKey) {
    const key = target.dataset.objectKey;
    const field = target.dataset.objectField;
    if (!state.moduleDraft[key] || typeof state.moduleDraft[key] !== 'object') state.moduleDraft[key] = {};
    state.moduleDraft[key][field] = Number(target.value || 0);
    setModuleDraftText();
    return;
  }

  const key = target.dataset.configKey;
  if (!key || !state.moduleDraft) return;
  let value;
  if (target.type === 'checkbox') value = target.checked;
  else if (target.type === 'number') value = Number(target.value || 0);
  else if (target.dataset.configMode === 'number-list') value = target.value.split(',').map(x => Number(x.trim())).filter(Number.isFinite);
  else if (target.dataset.configMode === 'line-list') value = target.value.split(/\r?\n|,/).map(x => x.trim()).filter(Boolean);
  else if (target.tagName === 'TEXTAREA') value = target.value.trim() ? JSON.parse(target.value) : null;
  else value = target.value;
  state.moduleDraft[key] = value;
  setModuleDraftText();
}

function handleBattlepassItemsClick(evt) {
  const add = evt.target.closest && evt.target.closest('[data-battlepass-item-add]');
  const remove = evt.target.closest && evt.target.closest('[data-battlepass-item-remove]');
  if (!add && !remove) return false;
  evt.preventDefault();
  const source = add || remove;
  const key = source.dataset.arrayKey;
  const index = Number(source.dataset.arrayIndex);
  const ctx = arrayTargetRow(source, key, index);
  if (!ctx.row) return true;
  const reward = ctx.row;
  const items = battlepassRewardItems(reward);
  if (add) {
    items.push({ itemId: 'Apple_2', quantity: 1 });
  } else {
    const itemIndex = Number(remove.dataset.battlepassItemRemove);
    if (Number.isInteger(itemIndex) && itemIndex >= 0) items.splice(itemIndex, 1);
  }
  setBattlepassRewardItems(reward, items);
  markArrayTargetChanged(ctx);
  renderModuleFields(state.moduleDraft);
  return true;
}

function normalizeWargmConfigForSave(config) {
  const clone = JSON.parse(JSON.stringify(config || {}));
  const isGameStores = String(state.selectedModule || '').toLowerCase() === 'gamestores-shop';
  const rulesKey = Array.isArray(clone.Rules) ? 'Rules' : (Array.isArray(clone.rules) ? 'rules' : '');
  if (!rulesKey) return clone;
  clone[rulesKey].forEach(rule => {
    if (!rule || typeof rule !== 'object') return;
    const objectId = String(
      wargmRuleValue(rule, 'MatchItemId', '') ||
      wargmRuleValue(rule, 'MatchObjectId', '') ||
      wargmRuleValue(rule, 'MatchProductId', '') ||
      ''
    ).trim();
    if (objectId) {
      const preferred = isGameStores && wargmRuleValue(rule, 'MatchProductId', '') ? 'MatchProductId' : 'MatchItemId';
      const actual = Object.keys(rule).find(key => key.toLowerCase() === preferred.toLowerCase()) || preferred;
      rule[actual] = objectId;
    } else {
      for (const optionalKey of ['MatchItemId', 'matchItemId', 'MatchObjectId', 'matchObjectId', 'MatchGoodsId', 'matchGoodsId', 'MatchProductId', 'matchProductId']) {
        const actual = Object.keys(rule).find(key => key.toLowerCase() === optionalKey.toLowerCase());
        if (actual && rule[actual] == null) rule[actual] = '';
      }
    }
    const mode = wargmRuleMode(rule);
    if (mode === 'Fame') {
      if (Object.prototype.hasOwnProperty.call(rule, 'deliveryMode') && !Object.prototype.hasOwnProperty.call(rule, 'DeliveryMode')) {
        rule.deliveryMode = 'Fame';
        delete rule.uiDeliveryMode;
        delete rule.commandTemplate;
      } else {
        rule.DeliveryMode = 'Fame';
        delete rule.UiDeliveryMode;
        delete rule.CommandTemplate;
      }
    } else {
      delete rule.UiDeliveryMode;
      delete rule.uiDeliveryMode;
    }
  });
  return clone;
}

function normalizeFastTravelConfigForSave(config) {
  const clone = JSON.parse(JSON.stringify(config || {}));
  const outpostsKey = Array.isArray(clone.Outposts) ? 'Outposts' : (Array.isArray(clone.outposts) ? 'outposts' : '');
  if (!outpostsKey) return clone;
  clone[outpostsKey].forEach(route => {
    if (!route || typeof route !== 'object') return;
    const arrivalKey = Array.isArray(route.ArrivalPoint) ? 'ArrivalPoint' : (Array.isArray(route.arrivalPoint) ? 'arrivalPoint' : 'ArrivalPoint');
    const centerKey = Array.isArray(route.CenterZone) ? 'CenterZone' : (Array.isArray(route.centerZone) ? 'centerZone' : 'CenterZone');
    const arrival = Array.isArray(route[arrivalKey]) ? route[arrivalKey] : [0, 0, 50];
    const x = Number(arrival[0] ?? 0);
    const y = Number(arrival[1] ?? 0);
    const z = Number(arrival[2] ?? 50);
    route[arrivalKey] = [Number.isFinite(x) ? x : 0, Number.isFinite(y) ? y : 0, Number.isFinite(z) ? z : 50];
    const existingCenter = route[centerKey];
    const hasValidCenter = Array.isArray(existingCenter) && existingCenter.length >= 2 &&
      Number.isFinite(Number(existingCenter[0])) && Number.isFinite(Number(existingCenter[1]));
    if (!hasValidCenter) {
      route[centerKey] = [route[arrivalKey][0], route[arrivalKey][1]];
    }
  });
  return clone;
}

function syncPortalFields(portal) {
  if (!portal) return;
  portal.querySelectorAll('input, textarea, select').forEach(field => {
    syncModuleField(field);
  });
}

function flushOpenModuleEditorsForSave() {
  const simpleSettingPortal = document.querySelector('[data-simple-setting-portal="true"]');
  if (simpleSettingPortal && state.simpleSettingEditPath) {
    const input = simpleSettingPortal.querySelector('[data-simple-setting-path]');
    if (input) syncSimpleSettingInput(input, true);
  }

  const simpleItemPortal = document.querySelector('[data-simple-item-portal="true"]');
  if (simpleItemPortal && state.simpleModuleEditDraft) {
    syncPortalFields(simpleItemPortal);
    commitSimpleModuleItemDraft({ reset: false, render: false, message: false });
  }

  setModuleDraftText();
}

const commandAliasConfigKeys = [
  'Help', 'Info', 'Hello', 'Language',
  'SetHome', 'Home', 'Homes', 'DeleteHome',
  'PrivateMessage', 'Reply', 'PrivateMessageHistory',
  'SectorScan', 'FastTravel', 'BaseLoot', 'Rent',
  'WelcomePack', 'DailyPack', 'Battlepass', 'Dlc', 'GameStores',
  'MoneyTransfer', 'ItemUpgrade', 'Quest',
  'Streak', 'Bounty', 'HunterTop', 'Wargm',
  'InventoryDelete', 'Vip', 'DiscordTest',
  'Armory', 'ArmoryBack', 'Editor'
];

function normalizeCommandAliasToken(value) {
  let text = String(value || '').trim().toLowerCase().replace(/\s+/g, '');
  if (!text) return '';
  const first = text[0];
  if (first === '/' || first === '!') {
    const body = text.slice(1).replace(/^[!/]+/, '');
    if (!body) return '';
    return first === '!' ? `!${body}` : body;
  }
  return text.replace(/^[!/]+/, '');
}

function normalizeCommandAliasList(value) {
  const raw = Array.isArray(value)
    ? value
    : String(value || '').split(/\r?\n|,|;/);
  const seen = new Set();
  const out = [];
  raw.forEach(item => {
    const alias = normalizeCommandAliasToken(item);
    if (!alias || seen.has(alias)) return;
    seen.add(alias);
    out.push(alias);
  });
  return out;
}

function normalizeCommandAliasesConfigForSave(rawConfig) {
  const config = Object.assign({}, rawConfig || {});
  commandAliasConfigKeys.forEach(key => {
    if (Object.prototype.hasOwnProperty.call(config, key)) {
      config[key] = normalizeCommandAliasList(config[key]);
    }
  });
  return config;
}

async function saveSelectedModule() {
  const selected = state.modules.find(module => module && module.key === state.selectedModule);
  const selectedKey = String(state.selectedModule || '').trim().toLowerCase();
  const baseline = state.moduleConfigBaselines[selectedKey] || '';
  if (!selected || selected.synthetic || !baseline) {
    throw new Error('Сохранение заблокировано: конфиг модуля не был успешно загружен с сервера. Обновите список модулей.');
  }

  const current = await api(`/api/plugin-config?name=${encodeURIComponent(state.selectedModule)}`);
  const currentConfig = current && current.config ? current.config : current;
  if (!currentConfig || moduleConfigFingerprint(currentConfig) !== baseline) {
    throw new Error('Сохранение отменено: конфиг модуля изменился после загрузки. Обновите модуль и внесите правку заново.');
  }

  flushOpenModuleEditorsForSave();
  const rawConfig = JSON.parse(els.moduleConfig.value);
  let config = rawConfig;
  if (isExternalShopModule()) config = normalizeWargmConfigForSave(rawConfig);
  if (state.selectedModule === 'scheduled-events') config = normalizeSchedulerConfigForSave(rawConfig);
  if (state.selectedModule === 'fast-travel') config = normalizeFastTravelConfigForSave(rawConfig);
  if (String(state.selectedModule || '').toLowerCase() === 'command-aliases') config = normalizeCommandAliasesConfigForSave(rawConfig);
  if (isChatResponseModuleKey(selectedKey)) validateChatResponseDraft(config);
  els.moduleConfig.value = JSON.stringify(config, null, 2);
  state.moduleDraft = config;
  const data = await api('/api/plugin-config', { method: 'POST', body: JSON.stringify({ name: state.selectedModule, config }) });
  state.moduleConfigBaselines[selectedKey] = moduleConfigFingerprint(config);
  await refreshModules();
  return data;
}

async function refreshDiagnostics() {
  const status = await api('/api/status').catch(err => ({
    bridge: { online: false, message: err && err.message ? err.message : 'status unavailable' },
    database: { online: false },
    logs: { online: false },
    error: err && err.message ? err.message : String(err || 'status unavailable')
  }));
  const queue = await api('/api/queue').catch(err => ({
    available: false,
    depth: 0,
    pending: 0,
    message: err && err.message ? err.message : 'queue status unavailable'
  }));
  const bridgeOnline = !!(status && status.bridge && status.bridge.online);
  updateDiagnosticSupport(status, detectDiagnosticsProblem(status, bridgeOnline));
  renderDiagnosticSummary(status, queue);
  const statusText = bridgeOnline
    ? 'Мост отвечает. Полные server/UE4SS/bridge логи доступны только через кнопку "Скачать логи", чтобы не нагружать сервер автоматическим чтением.'
    : `Мост не отвечает. ${status && status.bridge && status.bridge.message ? status.bridge.message : 'Проверьте загрузку bridge и скачайте диагностический пакет.'}`;
  const queueText = queue && queue.message
    ? `${statusText}\nОчередь: ${queue.message}`
    : statusText;
  if (els.queueStatus) els.queueStatus.textContent = queueText;
  if (els.traceLog) els.traceLog.textContent = statusText;
  if (els.serverLog) els.serverLog.textContent = 'Авточтение SCUM.log отключено. Используйте "Скачать логи" для ручной отправки в Discord.';
  if (els.runtimeLog) els.runtimeLog.textContent = 'Авточтение UE4SS/runtime логов отключено. Используйте "Скачать логи" для ручной отправки в Discord.';
  if (els.actionLog) els.actionLog.textContent = 'Журнал действий не читается автоматически на этой странице. Состояние модулей и события доступны в соответствующих разделах.';
}

function renderDiagnosticSummary(status, queue) {
  const bridge = status && status.bridge ? status.bridge : {};
  const queueDepth = Number(queue && queue.depth || 0);
  const pending = Number(queue && queue.pending || 0);
  const current = queue && queue.current && queue.current.label ? queue.current.label : 'idle';
  const last = queue && queue.lastDelivery ? queue.lastDelivery : null;
  if (els.diagBridge) {
    els.diagBridge.textContent = bridge.online ? 'online' : 'offline';
    els.diagBridge.className = bridge.online ? 'ok-text' : 'bad-text';
  }
  if (els.diagHeartbeat) els.diagHeartbeat.textContent = bridge.heartbeatAgeSeconds == null ? '-' : `${bridge.heartbeatAgeSeconds}s`;
  if (els.diagQueue) els.diagQueue.textContent = `${queueDepth} / pending ${pending}`;
  if (els.diagCurrent) els.diagCurrent.textContent = current;
  if (els.diagLastDelivery) {
    if (last && typeof last === 'object') {
      els.diagLastDelivery.textContent = `${last.action || 'delivery'}: ${last.ok ? 'ok' : 'fail'} ${last.message || ''}`.trim();
      els.diagLastDelivery.className = last.ok ? 'ok-text' : 'bad-text';
    } else {
      els.diagLastDelivery.textContent = 'none';
      els.diagLastDelivery.className = '';
    }
  }
}

function smokeTargetBody(extra = {}) {
  return Object.assign(serviceTargetBody(), extra);
}

function refreshCurrent() {
  refreshStatus().catch(toast);
  if (state.page === 'servers') {
    refreshServers().catch(toast);
    refreshServerControlStatus().catch(toast);
    refreshServerConfigs().catch(toast);
    refreshEvents().catch(toast);
  }
  if (state.page === 'players') refreshPlayers().catch(toast);
  if (state.page === 'squads') refreshSquads().catch(toast);
  if (state.page === 'chat') refreshChat().catch(toast);
  if (state.page === 'kills') refreshKills().catch(toast);
  if (state.page === 'modules') refreshModules().catch(toast);
  if (state.page === 'downloads') refreshDownloads().catch(toast);
}

function pick(obj, keys, fallback = '') {
  for (const key of keys) if (obj && obj[key] !== undefined && obj[key] !== null && obj[key] !== '') return obj[key];
  return fallback;
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[ch]));
}

function escapeAttr(value) {
  return escapeHtml(value).replace(/`/g, '&#96;');
}

function toast(err) {
  const msg = err && err.message ? err.message : String(err);
  if (els.quickResult) {
    if (els.quickResult.dataset) els.quickResult.dataset.i18nResultSource = msg;
    els.quickResult.textContent = translatePanelText(msg);
  }
}

function confirmDanger(message) {
  return window.confirm(translatePanelText(message));
}

function normalizeTargetText(target) {
  const value = String(target || '').trim();
  const lower = value.toLowerCase();
  if (['name', 'steamid', 'steam_id', 'targetname', 'targetsteamid', 'player'].includes(lower)) return '';
  return value;
}

function playerTargetBody(target) {
  target = normalizeTargetText(target);
  const lowerTarget = target.toLowerCase();
  const matched = (state.players || []).find(p => {
    const steamId = String(p.steamId || p.SteamId || p.steam || '');
    const name = String(p.name || p.Name || p.playerName || '');
    const runtimeKey = String(p.runtimeKey || p.RuntimeKey || '');
    return target && (steamId === target || name.toLowerCase() === lowerTarget || runtimeKey === target);
  }) || null;
  return {
    steamId: matched ? (matched.steamId || matched.SteamId || matched.steam || null) : (/^\d{16,20}$/.test(target) ? target : null),
    name: matched ? (matched.name || matched.Name || matched.playerName || null) : (/^\d{16,20}$/.test(target) ? null : target),
    runtimeKey: matched ? (matched.runtimeKey || matched.RuntimeKey || null) : null
  };
}

function selectedPlayerBody() {
  if (!state.selectedPlayerTarget) return null;
  return {
    steamId: state.selectedPlayerTarget.steamId || null,
    name: state.selectedPlayerTarget.name || null,
    runtimeKey: state.selectedPlayerTarget.runtimeKey || null,
    profileId: state.selectedPlayerTarget.profileId || null,
    userProfileId: state.selectedPlayerTarget.profileId || null,
    serverUserProfileId: state.selectedPlayerTarget.profileId || null
  };
}

async function loadPlayerFacts(steamId, name, runtimeKey = '', force = false) {
  if (typeof runtimeKey === 'boolean') {
    force = runtimeKey;
    runtimeKey = '';
  }
  if (!els.playerStatsBody && !els.playerFactsBody && !els.playerCharacterFactsBody && !els.playerEconomyFactsBody) return null;
  const key = playerInventoryKey(steamId, name, runtimeKey);
  const cached = state.playerFacts[key];
  if (cached && !force && !cached.loading) {
    renderPlayerFactsPanel();
    return cached;
  }
  state.playerFacts[key] = Object.assign({}, cached || {}, { loading: true, error: null });
  renderPlayerFactsPanel();
  const query = `steamId=${encodeURIComponent(steamId || '')}&name=${encodeURIComponent(name || '')}&runtimeKey=${encodeURIComponent(runtimeKey || '')}`;
  const [wallet, attributes, skills] = await Promise.all([
    api(`/api/player/wallet?${query}`).catch(err => ({ error: err && err.message ? err.message : String(err) })),
    api(`/api/player/attributes?${query}`).catch(err => ({ error: err && err.message ? err.message : String(err) })),
    api(`/api/player/skills?${query}`).catch(err => ({ error: err && err.message ? err.message : String(err) }))
  ]);
  state.playerFacts[key] = { loading: false, wallet, attributes, skills, fetchedAt: new Date().toISOString() };
  renderPlayerFactsPanel();
  return state.playerFacts[key];
}

function renderPlayerFactsPanel() {
  renderPlayerStatsPanel();
  renderPlayerCharacterFactsPanel();
  renderPlayerEconomyFactsPanel();
  if (!els.playerFactsBody || !state.selectedPlayerTarget) return;
  const { steamId, name, runtimeKey } = state.selectedPlayerTarget;
  const facts = state.playerFacts[playerInventoryKey(steamId, name, runtimeKey)];
  if (!facts || facts.loading) {
    els.playerFactsBody.innerHTML = '<div class="muted-line">Читаю кошелёк, славу и атрибуты...</div>';
    return;
  }
  const wallet = facts.wallet || {};
  const attrs = facts.attributes || {};
  const skills = facts.skills || {};
  const walletError = wallet.error ? `<div class="inventory-errors"><span>Кошелёк: ${escapeHtml(wallet.error)}</span></div>` : '';
  const attrsError = attrs.error ? `<div class="inventory-errors"><span>Атрибуты: ${escapeHtml(attrs.error)}</span></div>` : '';
  els.playerFactsBody.innerHTML = `
    <div class="inventory-summary">
      <article><b>${escapeHtml(wallet.normalBalance ?? wallet.moneyBalance ?? '-')}</b><span>Баланс</span></article>
      <article><b>${escapeHtml(wallet.goldBalance ?? 0)}</b><span>Золото</span></article>
      <article><b>${escapeHtml(wallet.famePoints ?? '-')}</b><span>Слава</span></article>
      <article><b>${escapeHtml(attrs.strength ?? '-')} / ${escapeHtml(attrs.constitution ?? '-')} / ${escapeHtml(attrs.dexterity ?? '-')} / ${escapeHtml(attrs.intelligence ?? '-')}</b><span>STR CON DEX INT</span></article>
    </div>
    ${walletError}${attrsError}`;
}

function renderPlayerCharacterFactsPanel() {
  if (!els.playerCharacterFactsBody || !state.selectedPlayerTarget) return;
  const { steamId, name, runtimeKey } = state.selectedPlayerTarget;
  const facts = state.playerFacts[playerInventoryKey(steamId, name, runtimeKey)];
  if (!facts || facts.loading) {
    els.playerCharacterFactsBody.innerHTML = '<div class="muted-line">Данные персонажа не читаются в фоне. Открой вкладку и нажми обновить, когда игрок уже прогрузился.</div>';
    return;
  }
  const attrs = facts.attributes || {};
  const errors = [
    attrs.error ? `Атрибуты: ${attrs.error}` : ''
  ].filter(Boolean);
  const strength = statValueFrom([attrs, facts], ['strength', 'Strength', 'str', 'STR']);
  const constitution = statValueFrom([attrs, facts], ['constitution', 'Constitution', 'con', 'CON']);
  const dexterity = statValueFrom([attrs, facts], ['dexterity', 'Dexterity', 'dex', 'DEX']);
  const intelligence = statValueFrom([attrs, facts], ['intelligence', 'Intelligence', 'int', 'INT']);
  const attributesHtml = [
    attributeBox('STR', 'Сила', strength),
    attributeBox('CON', 'Телосложение', constitution),
    attributeBox('DEX', 'Ловкость', dexterity),
    attributeBox('INT', 'Интеллект', intelligence)
  ].filter(Boolean).join('');
  els.playerCharacterFactsBody.innerHTML = `
    ${errors.length ? `<div class="inventory-errors">${errors.map(err => `<span>${escapeHtml(err)}</span>`).join('')}</div>` : ''}
    ${attributesHtml ? `<section class="player-stats-section compact-character-section">
      <div class="player-stats-section-head"><h3>Атрибуты сейчас</h3><span>из игры</span></div>
      <div class="attributes-grid">${attributesHtml}</div>
    </section>` : '<div class="muted-line">Атрибуты пока не найдены.</div>'}
  `;
}

function renderPlayerEconomyFactsPanel() {
  if (!els.playerEconomyFactsBody || !state.selectedPlayerTarget) return;
  const { steamId, name, runtimeKey } = state.selectedPlayerTarget;
  const facts = state.playerFacts[playerInventoryKey(steamId, name, runtimeKey)];
  if (!facts || facts.loading) {
    els.playerEconomyFactsBody.innerHTML = '<div class="muted-line">Баланс не читается в фоне. Нажми "Обновить данные", когда нужно проверить начисление.</div>';
    return;
  }
  const wallet = facts.wallet || {};
  const walletError = wallet.error ? `<div class="inventory-errors"><span>Баланс: ${escapeHtml(wallet.error)}</span></div>` : '';
  const balance = wallet.normalBalance ?? wallet.moneyBalance;
  const metricHtml = [
    tacticalMetric('Баланс', hasStatValue(balance) ? formatStatNumber(balance) : '-'),
    tacticalMetric('Золото', hasStatValue(wallet.goldBalance) ? formatStatNumber(wallet.goldBalance) : '-'),
    tacticalMetric('Слава', hasStatValue(wallet.famePoints) ? formatStatNumber(wallet.famePoints, 2) : '-')
  ].join('');
  const fetched = facts.fetchedAt ? `<span>Обновлено: ${escapeHtml(formatShortDateTime(facts.fetchedAt))}</span>` : '';
  els.playerEconomyFactsBody.innerHTML = `
    <div class="tactical-metrics-grid economy-facts-money">${metricHtml}</div>
    ${fetched ? `<div class="economy-facts-note">${fetched}</div>` : ''}
    ${walletError}
  `;
}

function metricCard(label, value, tone = '') {
  const className = tone ? ` class="${escapeAttr(tone)}"` : '';
  return `<article${className}><b>${escapeHtml(value ?? 0)}</b><span>${escapeHtml(label)}</span></article>`;
}

function formatShortDateTime(raw) {
  const ms = typeof raw === 'number' ? raw * 1000 : eventTime({ timestampUtc: raw });
  if (!ms) return '-';
  try {
    return new Date(ms).toLocaleString('ru-RU', {
      day: '2-digit',
      month: '2-digit',
      year: '2-digit',
      hour: '2-digit',
      minute: '2-digit'
    });
  } catch (_) {
    return '-';
  }
}

function selectedIdentitySet(selected) {
  const live = selected ? findPlayerByIdentity(selected.steamId, selected.name, selected.runtimeKey) : null;
  return {
    steam: String(selected && selected.steamId || live && (live.steamId || live.SteamId || live.steam) || '').trim(),
    name: String(selected && selected.name || live && (live.name || live.Name || live.playerName) || '').trim().toLowerCase(),
    profile: String(selected && selected.profileId || live && (live.userProfileId || live.serverUserProfileId || live.profileId || live.ProfileId) || '').trim(),
    runtime: String(selected && selected.runtimeKey || live && (live.runtimeKey || live.RuntimeKey) || '').trim()
  };
}

function valueFromKeys(obj, keys) {
  if (!obj || typeof obj !== 'object') return '';
  for (const key of keys) {
    const value = obj[key];
    if (value !== undefined && value !== null && String(value).trim() !== '') return value;
  }
  const lowerMap = new Map(Object.keys(obj).map(key => [String(key).toLowerCase(), key]));
  for (const key of keys) {
    const realKey = lowerMap.get(String(key).toLowerCase());
    if (!realKey) continue;
    const value = obj[realKey];
    if (value !== undefined && value !== null && String(value).trim() !== '') return value;
  }
  return '';
}

function normalizePlayerNameText(value) {
  return String(value || '')
    .replace(/\[[^\]]+\]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .toLowerCase();
}

function looseNameMatches(left, right) {
  const a = normalizePlayerNameText(left);
  const b = normalizePlayerNameText(right);
  if (!a || !b) return false;
  if (a === b) return true;
  return (a.length >= 3 && b.includes(a)) || (b.length >= 3 && a.includes(b));
}

function killTextParticipant(event, role) {
  const text = String(
    pick(event || {}, ['message', 'broadcastMessage', 'raw', 'text', 'line'], '') ||
    killEventTextBlob(event)
  ).replace(/\s+/g, ' ').trim();
  if (!text) return '';
  const patterns = [
    { re: /(?:^|\])\s*(.+?)\s+убил(?:а)?\s+(.+?)(?:\s+с\s+|\s+из\s+|\s+\[|,\s*Weapon|,\s*Distance|$)/i, killer: 1, victim: 2 },
    { re: /['"]?([^'"]+?)['"]?\s+was killed by\s+['"]?([^'"]+?)['"]?(?:$|\s|,|\.)/i, killer: 2, victim: 1 },
    { re: /(?:^|\])\s*(.+?)\s+killed\s+(.+?)(?:\s+with\s+|\s+\[|,\s*Weapon|,\s*Distance|$)/i, killer: 1, victim: 2 },
    { re: /killer\s*[:=]\s*([^,\]|]+).*?victim\s*[:=]\s*([^,\]|]+)/i, killer: 1, victim: 2 }
  ];
  for (const pattern of patterns) {
    const re = pattern.re;
    const match = text.match(re);
    if (!match) continue;
    const index = (role === 'victim' || role === 'target') ? pattern.victim : pattern.killer;
    return cleanKillParticipantName(match[index].trim());
  }
  return '';
}

function eventMatchesSelected(event, selected, role = '') {
  const id = selectedIdentitySet(selected);
  const prefix = role ? `${role}` : '';
  const steamKeys = role
    ? [`${prefix}SteamId`, `${prefix}SteamID`, `${prefix}Steam`, `${prefix}_steam_id`, `${prefix}steamId`]
    : ['steamId', 'SteamId', 'steam', 'targetSteamId', 'targetSteam', 'playerSteamId', 'actorSteamId', 'sourceSteamId', 'recipientSteamId'];
  const nameKeys = role
    ? [`${prefix}Name`, `${prefix}`, `${prefix}_name`]
    : ['name', 'Name', 'player', 'playerName', 'nickname', 'author', 'source', 'targetName', 'target', 'actorName', 'recipientName'];
  const profileKeys = role
    ? [`${prefix}UserProfileId`, `${prefix}ProfileId`, `${prefix}DbId`, `${prefix}_profile_id`]
    : ['userProfileId', 'serverUserProfileId', 'profileId', 'ProfileId', 'playerProfileId', 'targetProfileId'];
  const runtimeKeys = ['runtimeKey', 'RuntimeKey', 'targetRuntimeKey'];
  const eventSteam = String(valueFromKeys(event, steamKeys)).trim();
  const eventName = String(valueFromKeys(event, nameKeys)).trim().toLowerCase();
  const eventProfile = String(valueFromKeys(event, profileKeys)).trim();
  const eventRuntime = String(valueFromKeys(event, runtimeKeys)).trim();
  if (id.steam && eventSteam && id.steam === eventSteam) return true;
  if (id.profile && eventProfile && id.profile === eventProfile) return true;
  if (id.runtime && eventRuntime && id.runtime === eventRuntime) return true;
  if (id.name && eventName && looseNameMatches(id.name, eventName)) return true;
  if (role) {
    const participant = killTextParticipant(event, role);
    if (id.name && participant && looseNameMatches(id.name, participant)) return true;
  } else {
    const blob = actionTextBlob(event).toLowerCase();
    if (id.steam && blob.includes(id.steam)) return true;
    if (id.name && blob && looseNameMatches(id.name, blob)) return true;
  }
  return false;
}

function actionTextBlob(event) {
  const parts = [
    event && event.action,
    event && event.message,
    event && event.text,
    event && event.body,
    event && event.type,
    event && event.command,
    event && event.player,
    event && event.target,
    event && event.source,
    event && event.details && JSON.stringify(event.details)
  ];
  return parts.filter(Boolean).join(' ');
}

function buildPlayerCombatStats(selected) {
  const events = collectKillEvents(false)
    .slice()
    .sort((a, b) => eventTime(a) - eventTime(b));
  let kills = 0;
  let deaths = 0;
  let current = 0;
  let best = 0;
  let maxDistance = 0;
  let lastKill = null;
  let lastDeath = null;
  const weapons = new Map();
  const involved = [];
  for (const event of events) {
    const asKiller = eventMatchesSelected(event, selected, 'killer') || eventMatchesSelected(event, selected, 'attacker');
    const asVictim = eventMatchesSelected(event, selected, 'victim') || eventMatchesSelected(event, selected, 'target');
    if (!asKiller && !asVictim) continue;
    involved.push(event);
    if (asKiller && !asVictim && String(event.type || '').toLowerCase() !== 'death') {
      kills += 1;
      current += 1;
      best = Math.max(best, current);
      lastKill = event;
      const weapon = parseKillWeapon(event);
      if (weapon) weapons.set(weapon, (weapons.get(weapon) || 0) + 1);
      const distance = Number(parseKillDistance(event));
      if (Number.isFinite(distance) && distance > maxDistance) maxDistance = distance;
    }
    if (asVictim) {
      deaths += 1;
      current = 0;
      lastDeath = event;
    }
  }
  const favoriteWeapon = [...weapons.entries()].sort((a, b) => b[1] - a[1])[0];
  return {
    sourceAvailable: events.length > 0,
    eventsChecked: events.length,
    involvedCount: involved.length,
    kills,
    deaths,
    current,
    best,
    maxDistance,
    lastKill,
    lastDeath,
    favoriteWeapon: favoriteWeapon ? favoriteWeapon[0] : ''
  };
}

function buildPlayerActivityStats(selected) {
  const actions = Array.isArray(state.actionLog) ? state.actionLog : [];
  const chats = Array.isArray(state.chat) ? state.chat : [];
  const relatedActions = actions.filter(event => eventMatchesSelected(event, selected));
  const relatedChats = chats.filter(event => eventMatchesSelected(event, selected));
  const lockRegex = /lockpick|picklock|lock\s*picked|lock\s*open|замк|замок|замка|взлом/i;
  const commandCount = relatedChats.filter(event => String(event.channel || '').toLowerCase() === 'command' || String(event.message || '').trim().startsWith('/')).length;
  const locks = relatedActions.concat(relatedChats).filter(event => lockRegex.test(actionTextBlob(event))).length;
  const grants = relatedActions.filter(event => /grant|deliver|item|welcome|daily|wargm|spawn/i.test(actionTextBlob(event))).length;
  const money = relatedActions.filter(event => /money|currency|balance|fame|gold|баланс|слава|золото/i.test(actionTextBlob(event))).length;
  const vehicles = relatedActions.filter(event => /vehicle|rental|rent|spawn-vehicle|транспорт|аренд/i.test(actionTextBlob(event))).length;
  return {
    actionsChecked: actions.length > 0 || chats.length > 0,
    commands: commandCount,
    lockpicks: locks,
    grants,
    money,
    vehicles,
    lastAction: relatedActions.sort((a, b) => eventTime(b) - eventTime(a))[0] || null
  };
}

function formatStatNumber(value, digits = 0) {
  const number = Number(value);
  if (!Number.isFinite(number)) return value === 0 ? '0' : '-';
  return number.toLocaleString('ru-RU', {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits
  });
}

function hasStatValue(value) {
  if (value === undefined || value === null) return false;
  const text = String(value).trim();
  return text !== '' && text !== '-' && text.toLowerCase() !== 'нет данных' && text.toLowerCase() !== 'не найден';
}

function optionalHtml(condition, html) {
  return condition ? html : '';
}

function formatCoordsShort(coords) {
  return `${formatStatNumber(Math.round(coords.x))}, ${formatStatNumber(Math.round(coords.y))}, ${formatStatNumber(Math.round(coords.z))}`;
}

function statHeroItem(label, value, note = '') {
  return `<article>
    <span>${escapeHtml(label)}</span>
    <b>${escapeHtml(value ?? '-')}</b>
    ${note ? `<small>${escapeHtml(note)}</small>` : ''}
  </article>`;
}

function statLine(label, value, tone = '') {
  const className = tone ? ` class="${escapeAttr(tone)}"` : '';
  return `<div${className}><span>${escapeHtml(label)}</span><b>${escapeHtml(value ?? '-')}</b></div>`;
}

function statValueFrom(sources, keys) {
  for (const source of sources || []) {
    const value = valueFromKeys(source || {}, keys);
    if (value !== undefined && value !== null && String(value).trim() !== '') return value;
  }
  return '';
}

function statPercent(value) {
  if (value === undefined || value === null || String(value).trim() === '') return null;
  const number = Number(value);
  if (!Number.isFinite(number)) return null;
  const normalized = number >= 0 && number <= 1 ? number * 100 : number;
  return Math.max(0, Math.min(100, Math.round(normalized)));
}

function statBar(label, value, className) {
  const percent = statPercent(value);
  if (percent === null) return '';
  const text = `${percent}%`;
  const width = percent === null ? 0 : percent;
  return `<div class="stat-bar-group">
    <div class="stat-bar-header"><span>${escapeHtml(label)}</span><b>${escapeHtml(text)}</b></div>
    <div class="stat-bar-bg"><div class="stat-bar-fill ${escapeAttr(className)}${percent === null ? ' no-data' : ''}" style="width:${width}%"></div></div>
  </div>`;
}

function attributeBox(code, label, value) {
  if (!hasStatValue(value)) return '';
  const clean = formatStatNumber(value, 1);
  return `<article class="attribute-box">
    <span class="attr-val">${escapeHtml(clean)}</span>
    <span class="attr-code">${escapeHtml(code)}</span>
    <small>${escapeHtml(label)}</small>
  </article>`;
}

function tacticalMetric(label, value, note = '', tone = '') {
  const className = tone ? ` tactical-metric ${escapeAttr(tone)}` : 'tactical-metric';
  return `<article class="${className}">
    <span>${escapeHtml(label)}</span>
    <b>${escapeHtml(value ?? '-')}</b>
    ${note ? `<small>${escapeHtml(note)}</small>` : ''}
  </article>`;
}

function noDataBlock(text = 'Нет данных.') {
  return `<div class="player-stat-empty">${escapeHtml(text)}</div>`;
}

function sourceMetric(label, value, known, note = '') {
  return tacticalMetric(label, known ? value : 'нет данных', known ? note : '', known ? '' : 'muted');
}

function techInfoItem(label, value) {
  return `<div><span>${escapeHtml(label)}</span><b>${escapeHtml(value ?? '-')}</b></div>`;
}

function skillLevelLabel(level) {
  const n = Number(level);
  if (!Number.isFinite(n)) return '-';
  if (n >= 3) return 'Макс';
  if (n >= 2) return 'Средний';
  if (n >= 1) return 'Базовый';
  return 'Нет';
}

function renderSkillMatrix(skillsData) {
  const skills = Array.isArray(skillsData && skillsData.skills) ? skillsData.skills : [];
  if (!skills.length) {
    return '';
  }
  return `<div class="player-skills-grid">${skills.map(skill => {
    const rawLevel = Number(skill.level ?? skill.Level ?? 0);
    const displayLevel = Number.isFinite(rawLevel) && rawLevel >= 3 ? 4 : rawLevel;
    const width = Math.max(0, Math.min(100, (Number.isFinite(displayLevel) ? displayLevel : 0) / 4 * 100));
    const name = skill.name || skill.skillName || skill.dbName || '-';
    const experience = Number(skill.experience ?? skill.Experience ?? 0);
    return `<article class="player-skill-chip" title="DB level: ${escapeAttr(Number.isFinite(rawLevel) ? rawLevel : '-')}. В панели 3 отображается как игровой максимум 4.">
      <div>
        <b>${escapeHtml(name)}</b>
        <span>${escapeHtml(skillLevelLabel(rawLevel))}${Number.isFinite(experience) && experience > 0 ? ` · XP ${escapeHtml(Math.round(experience))}` : ''}</span>
      </div>
      <strong>${escapeHtml(Number.isFinite(displayLevel) ? displayLevel : '-')}</strong>
      <i><em style="width:${width}%"></em></i>
    </article>`;
  }).join('')}</div>`;
}

function statMini(label, value, note = '') {
  return `<article>
    <span>${escapeHtml(label)}</span>
    <b>${escapeHtml(value ?? 0)}</b>
    ${note ? `<small>${escapeHtml(note)}</small>` : ''}
  </article>`;
}

function statList(items, emptyText = 'Нет данных.') {
  const clean = (items || []).filter(Boolean);
  if (!clean.length && !emptyText) return '';
  if (!clean.length) return `<div class="player-stat-empty">${escapeHtml(emptyText)}</div>`;
  return `<div class="player-stat-list">${clean.join('')}</div>`;
}

function compactAssetLabel(obj, fallbackType = 'item') {
  const id = pick(obj || {}, ['itemId', 'ItemId', 'itemEntitySetup', 'itemClass', 'type', 'class', 'asset', 'vehicleId', 'VehicleId', 'vehicleType', 'vehicleAsset', '_table'], '');
  return id ? friendlyAssetName(id, fallbackType) : '-';
}

function worldObjectCoords(obj) {
  const x = Number(valueFromKeys(obj || {}, ['x', 'X', 'locationX', 'LocationX', 'coordX', 'posX', 'worldX']));
  const y = Number(valueFromKeys(obj || {}, ['y', 'Y', 'locationY', 'LocationY', 'coordY', 'posY', 'worldY']));
  const z = Number(valueFromKeys(obj || {}, ['z', 'Z', 'locationZ', 'LocationZ', 'coordZ', 'posZ', 'worldZ']));
  return [x, y, z].every(Number.isFinite) ? { x, y, z } : null;
}

function worldObjectCoordText(obj) {
  const coords = worldObjectCoords(obj);
  return coords ? formatCoordsShort(coords) : '';
}

function worldObjectIdText(obj) {
  const value = valueFromKeys(obj || {}, [
    'entityId', 'EntityId', 'vehicleEntityId', 'VehicleEntityId', 'vehicle_entity_id',
    'id', 'Id', 'itemEntityId', 'ItemEntityId'
  ]);
  return value ? `ID ${value}` : '';
}

function renderWorldObjectList(objects, fallbackType, emptyText) {
  const rows = (objects || []).filter(Boolean);
  if (!rows.length) return emptyText ? `<div class="player-stat-empty">${escapeHtml(emptyText)}</div>` : '';
  return `<div class="player-stat-list player-object-list">${rows.map(obj => {
    const id = worldObjectIdText(obj);
    const coords = worldObjectCoordText(obj);
    const details = [coords, id].filter(Boolean).join(' · ');
    return `<article>
      <b>${escapeHtml(compactAssetLabel(obj, fallbackType))}</b>
      ${details ? `<span>${escapeHtml(details)}</span>` : ''}
    </article>`;
  }).join('')}</div>`;
}

function objectMatchesSelected(obj, selected) {
  const id = selectedIdentitySet(selected);
  if (!obj || typeof obj !== 'object') return false;
  const steam = String(valueFromKeys(obj, [
    'steamId', 'SteamId', 'steam', 'ownerSteamId', 'OwnerSteamId', 'ownerSteam',
    'userSteamId', 'user_steam_id', 'lastOwnerSteamId', 'playerSteamId'
  ])).trim();
  const name = String(valueFromKeys(obj, [
    'name', 'Name', 'ownerName', 'OwnerName', 'playerName', 'targetName', 'createdByName', 'lastOwnerName'
  ])).trim().toLowerCase();
  const profile = String(valueFromKeys(obj, [
    'profileId', 'ProfileId', 'userProfileId', 'serverUserProfileId', 'ownerProfileId',
    'ownerUserProfileId', 'OwnerUserProfileId', 'ownerDbId', 'userId', 'UserId'
  ])).trim();
  if (id.steam && steam && id.steam === steam) return true;
  if (id.profile && profile && id.profile === profile) return true;
  if (id.name && name && id.name === name) return true;
  return false;
}

// vehicle-rentals is an append-only audit log.  The runtime cleanup uses the
// most recent row per player, so player statistics must not mistake an older
// `active` row for a vehicle that is still rented after a later expiry/return.
// Keep the raw history in state for audit screens; normalize only the view.
function currentVehicleRentalRecords(rows) {
  const latest = new Map();
  const anonymous = [];
  for (const row of rows || []) {
    if (!row || typeof row !== 'object') continue;
    const steamId = String(valueFromKeys(row, ['steamId', 'SteamId']) || '').trim();
    const name = String(valueFromKeys(row, ['name', 'Name']) || '').trim().toLowerCase();
    const key = steamId ? `steam:${steamId}` : (name ? `name:${name}` : '');
    if (!key) {
      anonymous.push(row);
      continue;
    }
    latest.set(key, row);
  }
  return anonymous.concat(Array.from(latest.values()));
}

function distance2d(a, b) {
  const ax = Number(a && (a.x ?? a.X ?? a.locationX ?? a.LocationX));
  const ay = Number(a && (a.y ?? a.Y ?? a.locationY ?? a.LocationY));
  const bx = Number(b && (b.x ?? b.X ?? b.locationX ?? b.LocationX));
  const by = Number(b && (b.y ?? b.Y ?? b.locationY ?? b.LocationY));
  if (![ax, ay, bx, by].every(Number.isFinite)) return Infinity;
  const dx = ax - bx;
  const dy = ay - by;
  return Math.sqrt(dx * dx + dy * dy);
}

function classifyContainerObject(obj) {
  const text = JSON.stringify(obj || {}).toLowerCase();
  const label = compactAssetLabel(obj, 'item').toLowerCase();
  const combined = `${text} ${label}`;
  const buried = /buried|burial|underground|закоп|bury/.test(combined);
  const chest = /chest|crate|box|сундук|ящик|container|storage/.test(combined);
  const locked = /lock|замок|locked|dial|code|gold|silver|iron/.test(combined);
  return { buried, chest, locked };
}

function buildPlayerWorldStats(selected, coords) {
  const mapLoaded = !!state.map;
  const map = state.map || {};
  const vehicles = Array.isArray(map.vehicles) ? map.vehicles : [];
  const rentals = currentVehicleRentalRecords(Array.isArray(state.vehicleRentals) ? state.vehicleRentals : []);
  const chests = Array.isArray(map.chests) ? map.chests : (Array.isArray(map.containers) ? map.containers : []);
  const flags = Array.isArray(map.flags) ? map.flags : [];
  const hasOwnerData = obj => !!valueFromKeys(obj || {}, [
    'steamId', 'SteamId', 'ownerSteamId', 'OwnerSteamId', 'ownerSteam',
    'userSteamId', 'user_steam_id', 'profileId', 'ProfileId', 'userProfileId',
    'ownerProfileId', 'ownerUserProfileId', 'ownerName', 'OwnerName', 'playerName'
  ]);
  const linkedDbVehicles = vehicles.filter(obj => objectMatchesSelected(obj, selected));
  const linkedRentalVehicles = rentals
    .filter(obj => objectMatchesSelected(obj, selected))
    .filter(obj => {
      const status = String(valueFromKeys(obj || {}, ['status', 'Status']) || '').toLowerCase();
      return !status || status === 'active' || status === 'pending' || status === 'verified';
    })
    .map(obj => Object.assign({}, obj, {
      asset: valueFromKeys(obj || {}, ['vehicleId', 'VehicleId', 'displayName', 'DisplayName', 'alias', 'Alias']),
      entityId: valueFromKeys(obj || {}, ['destroyRef', 'entityId', 'vehicleEntityId'])
    }));
  const rentalSeen = new Set();
  const compactRentalVehicles = linkedRentalVehicles
    .sort((a, b) => eventTime(b) - eventTime(a))
    .filter(obj => {
      const key = [
        valueFromKeys(obj, ['destroyRef', 'entityId', 'vehicleEntityId']),
        valueFromKeys(obj, ['startedAt', 'vehicleId', 'displayName']),
        Math.round(Number(valueFromKeys(obj, ['x', 'X']) || 0)),
        Math.round(Number(valueFromKeys(obj, ['y', 'Y']) || 0))
      ].join('|');
      if (rentalSeen.has(key)) return false;
      rentalSeen.add(key);
      return true;
    });
  const linkedVehicles = linkedDbVehicles.length ? linkedDbVehicles.concat(compactRentalVehicles) : compactRentalVehicles;
  const linkedChests = chests.filter(obj => objectMatchesSelected(obj, selected));
  const linkedFlags = flags.filter(obj => objectMatchesSelected(obj, selected));
  const nearVehicles = vehicles.filter(obj => !objectMatchesSelected(obj, selected) && distance2d(obj, coords) <= 12000);
  const nearChests = chests.filter(obj => !objectMatchesSelected(obj, selected) && distance2d(obj, coords) <= 12000);
  const linkedChestRows = linkedChests.map(obj => Object.assign({ __statsClass: classifyContainerObject(obj) }, obj));
  const classifiedChests = linkedChestRows.map(obj => obj.__statsClass);
  const classifiedNearChests = nearChests.map(classifyContainerObject);
  return {
    source: mapLoaded ? (map.chestSource || map.playerSource || 'карта') : '',
    mapLoaded,
    vehiclesTotal: vehicles.length,
    chestsTotal: chests.length,
    flagsTotal: flags.length,
    vehiclesWithOwner: vehicles.filter(hasOwnerData).length,
    chestsWithOwner: chests.filter(hasOwnerData).length,
    flagsWithOwner: flags.filter(hasOwnerData).length,
    linkedVehicles,
    linkedChests,
    linkedBuriedChests: linkedChestRows.filter(obj => obj.__statsClass && obj.__statsClass.buried),
    linkedStorageChests: linkedChestRows.filter(obj => obj.__statsClass && !obj.__statsClass.buried),
    linkedFlags,
    nearVehicles,
    nearChests,
    buriedChests: classifiedChests.filter(item => item.buried).length,
    lockedChests: classifiedChests.filter(item => item.locked).length,
    nearBuriedChests: classifiedNearChests.filter(item => item.buried).length,
    nearLockedChests: classifiedNearChests.filter(item => item.locked).length
  };
}

function classifyInventoryLocks(rows) {
  const counters = {
    total: 0,
    gold: 0,
    silver: 0,
    iron: 0,
    code: 0,
    keycard: 0,
    other: 0
  };
  const lockRows = [];
  for (const row of rows || []) {
    const raw = [
      row && row.itemId,
      row && row.itemEntitySetup,
      row && row.itemClass,
      row && row.slotKind,
      row && row.name
    ].filter(Boolean).join(' ');
    const lower = raw.toLowerCase();
    if (!/lock|padlock|dial|code|замок|keycard|bcu_lock|gold|silver|iron/.test(lower)) continue;
    counters.total += 1;
    if (/gold|золот/.test(lower)) counters.gold += 1;
    else if (/silver|сереб/.test(lower)) counters.silver += 1;
    else if (/iron|metal|желез/.test(lower)) counters.iron += 1;
    else if (/dial|code|combination|код/.test(lower)) counters.code += 1;
    else if (/keycard|card/.test(lower)) counters.keycard += 1;
    else counters.other += 1;
    lockRows.push(row);
  }
  return { counters, rows: lockRows };
}

function buildPlayerLockStats(selected, rows) {
  const inv = classifyInventoryLocks(rows);
  const actions = Array.isArray(state.actionLog) ? state.actionLog : [];
  const chats = Array.isArray(state.chat) ? state.chat : [];
  const related = actions.concat(chats).filter(event => eventMatchesSelected(event, selected));
  const opened = { total: 0, gold: 0, silver: 0, iron: 0, code: 0, keycard: 0, other: 0, player: 0, world: 0, unknown: 0 };
  const lockEvents = [];
  for (const event of related) {
    const text = actionTextBlob(event).toLowerCase();
    if (!/lockpick|picklock|lock\s*picked|unlock|замк|замок|взлом|отмыч/.test(text)) continue;
    opened.total += 1;
    if (/gold|золот/.test(text)) opened.gold += 1;
    else if (/silver|сереб/.test(text)) opened.silver += 1;
    else if (/iron|metal|желез/.test(text)) opened.iron += 1;
    else if (/dial|code|combination|код/.test(text)) opened.code += 1;
    else if (/keycard|card|ключ-карт|карта/.test(text)) opened.keycard += 1;
    else opened.other += 1;
    if (/player|prisoner|игрок/.test(text)) opened.player += 1;
    else if (/world|door|container|base|мир|двер|сундук|ящик|база/.test(text)) opened.world += 1;
    else opened.unknown += 1;
    lockEvents.push(event);
  }
  return { inventory: inv, opened, events: lockEvents.sort((a, b) => eventTime(b) - eventTime(a)).slice(0, 5) };
}

function renderPlayerStatsPanel() {
  if (!els.playerStatsBody || !state.selectedPlayerTarget) return;
  const selected = state.selectedPlayerTarget;
  const player = findPlayerByIdentity(selected.steamId, selected.name, selected.runtimeKey) || {};
  const key = playerInventoryKey(selected.steamId, selected.name, selected.runtimeKey);
  const facts = state.playerFacts[key] || {};
  const inv = state.playerInventory[key] || {};
  const wallet = facts.wallet || {};
  const attrs = facts.attributes || {};
  const skills = facts.skills || {};
  const rows = Array.isArray(inv.rows) ? inv.rows : [];
  const coordSource = Object.keys(player).length ? player : selected;
  const coordX = valueFromKeys(coordSource || {}, ['x', 'X', 'locationX', 'LocationX']);
  const coordY = valueFromKeys(coordSource || {}, ['y', 'Y', 'locationY', 'LocationY']);
  const coordsKnown = Number.isFinite(Number(coordX)) && Number.isFinite(Number(coordY));
  const coords = playerCoords(coordSource);
  const coordText = coordsKnown ? formatCoordsShort(coords) : '';
  const combat = buildPlayerCombatStats(selected);
  const activity = buildPlayerActivityStats(selected);
  const world = buildPlayerWorldStats(selected, coords);
  const lockStats = buildPlayerLockStats(selected, rows);
  const recentEvents = collectKillEvents(false)
    .filter(event => eventMatchesSelected(event, selected, 'killer') || eventMatchesSelected(event, selected, 'victim') || eventMatchesSelected(event, selected, 'attacker') || eventMatchesSelected(event, selected, 'target'))
    .sort((a, b) => eventTime(b) - eventTime(a))
    .slice(0, 4);
  const playerVehicles = world.linkedVehicles || [];
  const playerChests = world.linkedStorageChests || [];
  const playerBuried = world.linkedBuriedChests || [];
  const errors = [
    wallet.error ? `Кошелёк: ${wallet.error}` : '',
    attrs.error ? `Атрибуты: ${attrs.error}` : '',
    skills.error ? `Навыки: ${skills.error}` : '',
    inv.error ? `Инвентарь: ${inv.error}` : ''
  ].filter(Boolean);
  const dossierPills = [
    selected.steamId ? `<span>${escapeHtml(selected.steamId)}</span>` : '',
    selected.profileId ? `<span>профиль ${escapeHtml(selected.profileId)}</span>` : ''
  ].filter(Boolean).join('');
  const balanceValue = wallet.normalBalance ?? wallet.moneyBalance;
  const dossierKpis = [
    coordText ? tacticalMetric('Позиция', coordText) : '',
    hasStatValue(balanceValue) ? tacticalMetric('Баланс', formatStatNumber(balanceValue)) : '',
    hasStatValue(wallet.famePoints) ? tacticalMetric('Слава', formatStatNumber(wallet.famePoints, 2)) : '',
    hasStatValue(wallet.goldBalance) ? tacticalMetric('Золото', formatStatNumber(wallet.goldBalance)) : '',
    tacticalMetric('Транспорт', playerVehicles.length)
  ].filter(Boolean).join('');
  const strength = statValueFrom([attrs, player, facts], ['strength', 'Strength', 'str', 'STR']);
  const constitution = statValueFrom([attrs, player, facts], ['constitution', 'Constitution', 'con', 'CON']);
  const dexterity = statValueFrom([attrs, player, facts], ['dexterity', 'Dexterity', 'dex', 'DEX']);
  const intelligence = statValueFrom([attrs, player, facts], ['intelligence', 'Intelligence', 'int', 'INT']);
  const attributesHtml = [
    attributeBox('STR', 'Сила', strength),
    attributeBox('CON', 'Телосложение', constitution),
    attributeBox('DEX', 'Ловкость', dexterity),
    attributeBox('INT', 'Интеллект', intelligence)
  ].filter(Boolean).join('');
  const skillsHtml = renderSkillMatrix(skills);
  const combatSourceNote = combat.eventsChecked
    ? `\u0438\u0437 kill-\u043b\u043e\u0433\u043e\u0432: ${combat.eventsChecked}`
    : '\u0436\u0434\u0443 \u0441\u0432\u0435\u0436\u0438\u0435 kill-\u043b\u043e\u0433\u0438';
  const worldSourceNote = world.mapLoaded
    ? `карта: ${world.vehiclesTotal} \u0442\u0440\u0430\u043d\u0441\u043f., ${world.chestsTotal} \u043e\u0431\u044a.`
    : '\u0436\u0434\u0443 \u0434\u0430\u043d\u043d\u044b\u0435 \u043a\u0430\u0440\u0442\u044b';
  const lockSourceNote = (Array.isArray(state.actionLog) || Array.isArray(state.chat))
    ? `\u043b\u043e\u0433\u0438: ${lockStats.events.length} \u0441\u043e\u0431.`
    : '\u0436\u0434\u0443 \u043b\u043e\u0433\u0438';
  const tacticalMetrics = [
    tacticalMetric('\u0423\u0431\u0438\u0439\u0441\u0442\u0432\u0430', combat.kills, combatSourceNote),
    tacticalMetric('\u0421\u043c\u0435\u0440\u0442\u0438', combat.deaths, combatSourceNote),
    tacticalMetric('\u0421\u0435\u0440\u0438\u044f', combat.current, `\u043b\u0443\u0447\u0448\u0430\u044f: ${combat.best}`),
    combat.maxDistance > 0 ? tacticalMetric('\u041c\u0430\u043a\u0441. \u0434\u0438\u0441\u0442\u0430\u043d\u0446\u0438\u044f', `${Math.round(combat.maxDistance)} \u043c`) : '',
    tacticalMetric('\u0422\u0440\u0430\u043d\u0441\u043f\u043e\u0440\u0442', playerVehicles.length, worldSourceNote),
    tacticalMetric('\u0421\u0443\u043d\u0434\u0443\u043a\u0438', playerChests.length, worldSourceNote),
    tacticalMetric('\u0417\u0430\u043a\u043e\u043f\u043a\u0438', playerBuried.length, worldSourceNote),
    tacticalMetric('\u0412\u0441\u043a\u0440\u044b\u0442\u0438\u044f', lockStats.opened.total, lockSourceNote)
  ].filter(Boolean).join('');
  const combatLines = [
    combat.maxDistance > 0 ? statLine('Макс. дистанция', `${Math.round(combat.maxDistance)} м`) : '',
    combat.favoriteWeapon ? statLine('Оружие чаще всего', friendlyAssetName(combat.favoriteWeapon, 'item')) : '',
    combat.lastKill ? statLine('Последнее убийство', formatShortDateTime(eventTime(combat.lastKill) / 1000)) : '',
    combat.lastDeath ? statLine('Последняя смерть', formatShortDateTime(eventTime(combat.lastDeath) / 1000)) : ''
  ].filter(Boolean).join('');
  const openedLockStats = [
    ['Всего вскрыто', lockStats.opened.total],
    ['Золотые', lockStats.opened.gold],
    ['Серебряные', lockStats.opened.silver],
    ['Железные', lockStats.opened.iron],
    ['Кодовые', lockStats.opened.code],
    ['Карты / ключи', lockStats.opened.keycard],
    ['Прочие', lockStats.opened.other]
  ];
  const recentLockEvents = lockStats.events.map(event => `<article>
    <b>${escapeHtml(formatShortDateTime(eventTime(event) / 1000))}</b>
    <span>${escapeHtml(actionTextBlob(event).slice(0, 170) || 'событие замка')}</span>
  </article>`);
  const activityLines = [
    activity.lastAction ? statLine('Последнее действие панели', formatShortDateTime(eventTime(activity.lastAction) / 1000)) : '',
    activity.lastAction ? statLine('Текст последнего действия', actionTextBlob(activity.lastAction).slice(0, 180)) : ''
  ].filter(Boolean).join('');
  const recentKillHtml = recentEvents.map(event => {
    const weapon = parseKillWeapon(event) || '';
    const distance = parseKillDistance(event);
    const text = pick(event, ['message', 'broadcastMessage'], `${pick(event, ['killerName', 'killer'], '')} -> ${pick(event, ['victimName', 'victim'], '')}`);
    const meta = [
      weapon ? friendlyAssetName(weapon, 'item') : '',
      distance ? `${Math.round(Number(distance))} м` : ''
    ].filter(Boolean).join(' · ');
    return `<article><b>${escapeHtml(formatShortDateTime(eventTime(event) / 1000))}</b><span>${escapeHtml(text)}</span>${meta ? `<small>${escapeHtml(meta)}</small>` : ''}</article>`;
  }).join('');
  const activitySection = '';
  els.playerStatsBody.innerHTML = `
    <div class="player-stats-dashboard">
      <section class="player-dossier">
        <div class="player-dossier-main">
          <div class="player-dossier-avatar">${escapeHtml(String(selected.name || 'UN').slice(0, 2).toUpperCase())}</div>
          <div>
            <small>Досье игрока</small>
            <h3>${escapeHtml(selected.name || 'Игрок')}</h3>
            ${dossierPills ? `<div class="player-dossier-pills">${dossierPills}</div>` : ''}
          </div>
        </div>
        <div class="player-dossier-kpis">${dossierKpis}</div>
        ${errors.length ? `<div class="inventory-errors">${errors.map(err => `<span>${escapeHtml(err)}</span>`).join('')}</div>` : ''}
      </section>

      ${attributesHtml ? `<div class="player-stats-tactical-grid player-stats-character-grid">
        <section class="attributes-card player-stats-section">
          <div class="player-stats-section-head">
            <h3>Атрибуты</h3>
            <span>STR / CON / DEX / INT</span>
          </div>
          <div class="attributes-grid">${attributesHtml}</div>
        </section>
      </div>` : ''}

      ${skillsHtml ? `<section class="player-stats-section player-stats-section-wide player-skills-card">
        <div class="player-stats-section-head">
          <h3>Навыки персонажа</h3>
          <span>уровень и опыт из prisoner_skill</span>
        </div>
        ${skillsHtml}
      </section>` : ''}

      <section class="player-stats-section player-stats-section-wide">
        <div class="player-stats-section-head">
          <h3>Тактическая сводка</h3>
          <span>боевые итоги, имущество и вскрытия без технических заглушек</span>
        </div>
        <div class="tactical-metrics-grid">${tacticalMetrics}</div>
      </section>
    </div>

    <div class="player-stats-layout">
      <section class="player-stats-section">
        <div class="player-stats-section-head">
          <h3>Боевые показатели</h3>
          <span>киллфид и серии</span>
        </div>
        <div class="player-stat-scoreboard">
          ${statMini('\u0423\u0431\u0438\u0439\u0441\u0442\u0432\u0430', combat.kills, combatSourceNote)}
          ${statMini('\u0421\u043c\u0435\u0440\u0442\u0438', combat.deaths, combatSourceNote)}
          ${statMini('\u0422\u0435\u043a\u0443\u0449\u0430\u044f \u0441\u0435\u0440\u0438\u044f', combat.current)}
          ${statMini('\u041b\u0443\u0447\u0448\u0430\u044f \u0441\u0435\u0440\u0438\u044f', combat.best)}
        </div>
        ${combatLines ? `<div class="player-stat-lines">${combatLines}</div>` : ''}
      </section>

      <section class="player-stats-section">
        <div class="player-stats-section-head">
          <h3>Транспорт</h3>
          <span>только транспорт игрока и координаты, если они известны</span>
        </div>
        <div class="player-stat-scoreboard">${statMini('\u0422\u0440\u0430\u043d\u0441\u043f\u043e\u0440\u0442 \u0438\u0433\u0440\u043e\u043a\u0430', playerVehicles.length, worldSourceNote)}</div>
        ${renderWorldObjectList(playerVehicles, 'vehicle', '')}
      </section>

      <section class="player-stats-section">
        <div class="player-stats-section-head">
          <h3>Сундуки и закопки</h3>
          <span>количество и координаты объектов игрока</span>
        </div>
        <div class="player-stat-scoreboard">
          ${statMini('\u0421\u0443\u043d\u0434\u0443\u043a\u0438', playerChests.length, worldSourceNote)}
          ${statMini('\u0417\u0430\u043a\u043e\u043f\u043a\u0438', playerBuried.length, worldSourceNote)}
          ${statMini('\u0412\u0441\u0435\u0433\u043e', playerChests.length + playerBuried.length)}
        </div>
        ${playerChests.length ? `<div class="player-stat-subtitle">Сундуки</div>${renderWorldObjectList(playerChests, 'item', '')}` : ''}
        ${playerBuried.length ? `<div class="player-stat-subtitle">Закопки</div>${renderWorldObjectList(playerBuried, 'item', '')}` : ''}
      </section>

      <section class="player-stats-section">
        <div class="player-stats-section-head">
          <h3>\u0417\u0430\u043c\u043a\u0438 \u0438 \u0432\u0441\u043a\u0440\u044b\u0442\u0438\u044f</h3>
          <span>\u0442\u043e\u043b\u044c\u043a\u043e \u0432\u0441\u043a\u0440\u044b\u0442\u044b\u0435 \u0437\u0430\u043c\u043a\u0438 \u043f\u043e \u0442\u0438\u043f\u0430\u043c</span>
        </div>
        <div class="player-stat-scoreboard">
          ${openedLockStats.slice(0, 4).map(([label, value]) => statMini(label, value)).join('')}
        </div>
        <div class="player-stat-lines">
          ${openedLockStats.slice(4).map(([label, value]) => statLine(label, value)).join('')}
        </div>
        ${statList(recentLockEvents, '')}
      </section>

      ${activitySection}
    </div>`;
}

async function loadPlayerStats(steamId, name, runtimeKey = '', force = false) {
  renderPlayerStatsPanel();
  const tasks = [
    loadPlayerFacts(steamId, name, runtimeKey, force).catch(err => ({ error: err && err.message ? err.message : String(err) })),
    loadPlayerInventory(steamId, name, runtimeKey, force).catch(err => ({ error: err && err.message ? err.message : String(err) }))
  ];
  if (force || !Array.isArray(state.kills) || !state.kills.length) {
    tasks.push(api('/api/kills').then(data => {
      state.kills = normalizeKillEvents(arrayFromPayload(data, ['kills', 'events', 'recent', 'rows']));
    }).catch(() => {}));
  }
  if (force || !state.map) {
    tasks.push(api('/api/map').then(async data => {
      state.map = data || {};
      const apiChests = Array.isArray(state.map.chests)
        ? state.map.chests
        : (Array.isArray(state.map.containers) ? state.map.containers : []);
      if (!apiChests.length) {
        const staticChests = await loadStaticMapChests();
        if (staticChests.length) {
          state.map.chests = staticChests;
          state.map.chestSource = 'игровые данные';
        }
      }
    }).catch(() => {}));
  }
  if (force || !Array.isArray(state.vehicleRentals) || !state.vehicleRentals.length) {
    tasks.push(api('/api/module-state?key=vehicle-rental').then(data => {
      state.vehicleRentals = arrayFromPayload(data, ['rentals', 'vehicles', 'items', 'rows']);
    }).catch(() => {}));
  }
  if (force || !Array.isArray(state.chat) || !state.chat.length || !Array.isArray(state.actionLog) || !state.actionLog.length) {
    tasks.push(Promise.all([
      api('/api/chat?limit=300').then(data => { state.chat = arrayFromPayload(data, ['chat', 'messages', 'recent', 'events']); }).catch(() => {}),
      api('/api/action-log?limit=300').then(data => { state.actionLog = arrayFromPayload(data, ['actions', 'events', 'recent', 'rows']); }).catch(() => {})
    ]));
  }
  await Promise.all(tasks);
  renderPlayerStatsPanel();
}

async function loadPlayerInventory(steamId, name, runtimeKey = '', force = false) {
  if (typeof runtimeKey === 'boolean') {
    force = runtimeKey;
    runtimeKey = '';
  }
  const key = playerInventoryKey(steamId, name, runtimeKey);
  const cached = state.playerInventory[key];
  if (cached && !force && !cached.error && !cached.loading) {
    renderPlayerInventoryPanel();
    return cached;
  }
  state.playerInventory[key] = Object.assign({}, cached || {}, { loading: true, error: null });
  renderPlayerInventoryPanel();
  try {
    const data = await api(`/api/player-inventory?steamId=${encodeURIComponent(steamId || '')}&name=${encodeURIComponent(name || '')}&runtimeKey=${encodeURIComponent(runtimeKey || '')}`);
    state.playerInventory[key] = Object.assign({}, data || {}, { loading: false, error: null, fetchedAt: new Date().toISOString() });
  } catch (err) {
    state.playerInventory[key] = { loading: false, error: err && err.message ? err.message : String(err), rows: [], quickSlots: [] };
  }
  renderPlayerInventoryPanel();
  renderPlayerStatsPanel();
  return state.playerInventory[key];
}

function renderPlayerInventoryPanel() {
  if (!els.playerInventoryBody || !state.selectedPlayerTarget) return;
  const { steamId, name, runtimeKey } = state.selectedPlayerTarget;
  const inv = state.playerInventory[playerInventoryKey(steamId, name, runtimeKey)];
  if (!inv || inv.loading) {
    els.playerInventoryBody.innerHTML = '<div class="muted-line">Загружаю инвентарь...</div>';
    return;
  }
  if (inv.error) {
    els.playerInventoryBody.innerHTML = `<div class="event">${escapeHtml(inv.error)}</div>`;
    return;
  }
  const rows = Array.isArray(inv.rows) ? inv.rows : [];
  const quick = Array.isArray(inv.quickSlots) ? inv.quickSlots : [];
  const hands = rows.find(row => String(row.slotKind || '').toLowerCase() === 'hands');
  const errors = Array.isArray(inv.errors) ? inv.errors : [];
  els.playerInventoryBody.innerHTML = `
    <div class="inventory-block">
      <h3>В руках</h3>
      ${hands ? renderInventoryNode(hands, new Map()) : '<div class="muted-line">Пусто или слот не сохранён в базе.</div>'}
    </div>
    <div class="inventory-block">
      <h3>Быстрые слоты</h3>
      <div class="quick-slots">${quick.length ? quick.map(slot => {
        const id = slot.itemId || slot.itemEntitySetup || slot.itemClass || '';
        return `<span><b>${escapeHtml(slot.slotIndex ?? '-')}</b> ${escapeHtml(friendlyAssetName(id, 'item'))}</span>`;
      }).join('') : '<span>Нет записей</span>'}</div>
    </div>
    <div class="inventory-block">
      <h3>Экипировка и контейнеры</h3>
      <div class="inventory-tree">${renderInventoryTree(rows)}</div>
    </div>
    ${errors.length ? `<div class="inventory-errors">${errors.map(err => `<span>${escapeHtml(err.area || 'db')}: ${escapeHtml(err.message || '')}</span>`).join('')}</div>` : ''}`;
}

function refreshSelectedInventorySoon(delay = 1600) {
  const selected = state.selectedPlayerTarget;
  if (!selected) return;
  window.setTimeout(() => loadPlayerInventory(selected.steamId, selected.name, selected.runtimeKey, true).catch(toast), delay);
}

function refreshSelectedFactsSoon(delay = 1200) {
  const selected = state.selectedPlayerTarget;
  if (!selected) return;
  window.setTimeout(() => loadPlayerFacts(selected.steamId, selected.name, selected.runtimeKey, true).catch(toast), delay);
}

function serviceTargetBody() {
  return playerTargetBody((els.serviceTarget && els.serviceTarget.value || '').trim());
}

function serviceTargetPlayer() {
  const target = normalizeTargetText((els.serviceTarget && els.serviceTarget.value || '').trim());
  const lower = target.toLowerCase();
  if (!target) return null;
  return (state.players || []).find(player => {
    const steamId = String(player.steamId || player.SteamId || player.steam || '');
    const name = String(player.name || player.Name || player.playerName || '');
    const runtimeKey = String(player.runtimeKey || player.RuntimeKey || '');
    return steamId === target || name.toLowerCase() === lower || runtimeKey === target;
  }) || null;
}

function numberFromKeys(obj, keys) {
  for (const key of keys) {
    const value = Number(obj && obj[key]);
    if (Number.isFinite(value)) return value;
  }
  return null;
}

function playerXY(player) {
  if (!player) return null;
  const x = numberFromKeys(player, ['x', 'X', 'locationX', 'LocationX']);
  const y = numberFromKeys(player, ['y', 'Y', 'locationY', 'LocationY']);
  return Number.isFinite(x) && Number.isFinite(y) ? { x, y } : null;
}

function distance2dClient(ax, ay, bx, by) {
  const dx = Number(ax) - Number(bx);
  const dy = Number(ay) - Number(by);
  return Math.sqrt(dx * dx + dy * dy);
}

function selectedFastRoutePoint() {
  const opt = els.fastRoute && els.fastRoute.selectedOptions ? els.fastRoute.selectedOptions[0] : null;
  const parts = opt && opt.dataset.point ? opt.dataset.point.split(',').map(Number) : [];
  const routeX = parts.length >= 2 && Number.isFinite(parts[0]) ? parts[0] : Number(els.fastX && els.fastX.value || 0);
  const routeY = parts.length >= 2 && Number.isFinite(parts[1]) ? parts[1] : Number(els.fastY && els.fastY.value || 0);
  return { option: opt, x: routeX, y: routeY };
}

function fastTravelConfiguredPrice(route) {
  const explicit = els.fastFare && els.fastFare.value.trim() !== '' ? Number(els.fastFare.value) : null;
  if (Number.isFinite(explicit) && explicit > 0) return Math.max(0, Math.floor(explicit));
  const optionPrice = route && route.option && route.option.dataset.price !== '' ? Number(route.option.dataset.price) : null;
  if (Number.isFinite(optionPrice) && optionPrice > 0) return Math.max(0, Math.floor(optionPrice));
  const fast = moduleConfig('fast-travel');
  const fixed = Number(fast.FixedFare ?? fast.fixedFare ?? fast.TravelCost ?? fast.travelCost ?? 0);
  if (Number.isFinite(fixed) && fixed > 0) return Math.max(0, Math.floor(fixed));
  return null;
}

function updateFastTravelPricePreview() {
  if (!els.fastPricePreview) return;
  const route = selectedFastRoutePoint();
  const fixedPrice = fastTravelConfiguredPrice(route);
  const player = serviceTargetPlayer();
  const position = playerXY(player);
  const routeName = route.option ? route.option.textContent.trim() : 'ручная точка';
  if (fixedPrice !== null) {
    els.fastPricePreview.textContent = `Цена до "${routeName}": ${fixedPrice}.`;
    return;
  }
  if (!position) {
    els.fastPricePreview.textContent = 'Цена считается от позиции игрока: укажи онлайн-игрока сверху.';
    return;
  }
  if (!Number.isFinite(route.x) || !Number.isFinite(route.y)) {
    els.fastPricePreview.textContent = 'Укажи маршрут или координаты точки.';
    return;
  }
  const fast = moduleConfig('fast-travel');
  const rate = Number(fast.RatePerMeter ?? fast.ratePerMeter ?? 0.5);
  const fare = Math.max(0, Math.floor(distance2dClient(position.x, position.y, route.x, route.y) * (Number.isFinite(rate) ? rate : 0.5) + 0.5));
  els.fastPricePreview.textContent = `Цена до "${routeName}": ${fare}. Расстояние: ${Math.floor(distance2dClient(position.x, position.y, route.x, route.y) + 0.5)} см.`;
}

const wargmModeMeta = {
  item: {
    label: 'Предметы',
    hint: 'Для кейса добавь несколько предметов в набор. Если набор пуст, выдастся один предмет из поля ID.'
  },
  vehicle: {
    label: 'Транспорт',
    hint: 'Выдача транспорта ставится в live-очередь и выполняется рядом с выбранным игроком.'
  },
  cargodrop: {
    label: 'Cargo drop',
    hint: 'Вызывает SCUM cargo drop по live-координатам выбранного игрока через ScheduleWorldEvent BP_CargoDropEvent X= Y= Z=.'
  },
  vip: {
    label: 'VIP',
    hint: 'Выдаёт VIP-привилегию на указанное число дней. Если у игрока уже есть VIP, срок продлится от текущей даты окончания.'
  },
  money: {
    label: 'Баланс',
    hint: 'Изменяет обычный баланс игрока через безопасный bridge route.'
  },
  gold: {
    label: 'Золото',
    hint: 'Начисляет или списывает золото игроку через безопасный bridge route.'
  },
  fame: {
    label: 'Очки славы',
    hint: 'Начисляет или списывает очки славы поверх текущей суммы.'
  },
  skill: {
    label: 'Навык',
    hint: 'Выбери навык из каталога или введи точное имя, например Handgun или Melee Weapons.'
  },
  allskills: {
    label: 'Все навыки',
    hint: 'Выдаёт весь список навыков SCUM выбранному игроку. Для полной прокачки оставь уровень 4.'
  },
  attributes: {
    label: 'Атрибуты',
    hint: 'Укажи четыре значения персонажа: сила, тело, ловкость и интеллект.'
  },
  command: {
    label: 'Команда',
    hint: 'Для редких случаев. Используй только проверенные SCUM-команды.'
  }
};

function findCatalogItemLabel(itemId) {
  const key = String(itemId || '').trim().toLowerCase();
  if (!key) return '';
  const item = (state.items || []).find(row => String(row.itemId || row.id || '').toLowerCase() === key);
  if (!item) return '';
  return item.name || item.category || '';
}

function wargmTargetLabel() {
  const target = (els.serviceTarget && els.serviceTarget.value || '').trim();
  return target || 'цель не указана';
}

function buildWargmPreview() {
  const mode = els.wargmMode ? els.wargmMode.value : 'item';
  if (mode === 'item') {
    const items = state.wargmItems.length
      ? state.wargmItems
      : ((els.wargmItemId && els.wargmItemId.value.trim()) ? [{ ItemId: els.wargmItemId.value.trim(), Quantity: Number(els.wargmQty && els.wargmQty.value || 1) }] : []);
    const count = items.reduce((sum, item) => sum + Math.max(1, Number(item.Quantity || 1)), 0);
    return items.length
      ? `К выдаче: ${items.length} строк, ${count} шт. Цель: ${wargmTargetLabel()}.`
      : `Выбери предмет или собери набор. Цель: ${wargmTargetLabel()}.`;
  }
  if (mode === 'vehicle') return `Транспорт: ${(els.wargmVehicleId && els.wargmVehicleId.value.trim()) || 'не выбран'}. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'cargodrop') return `Cargo drop будет вызван рядом с игроком. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'vip') return `VIP на ${Number(els.wargmAmount && els.wargmAmount.value || 30)} дней. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'money') return `Баланс: ${Number(els.wargmAmount && els.wargmAmount.value || 0)}. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'gold') return `Золото: ${Number(els.wargmAmount && els.wargmAmount.value || 0)}. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'fame') return `Очки славы: ${Number(els.wargmAmount && els.wargmAmount.value || 0)}. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'skill') return `Навык: ${(els.wargmSkill && els.wargmSkill.value.trim()) || 'не выбран'}, уровень ${Number(els.wargmSkillLevel && els.wargmSkillLevel.value || 0)}. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'allskills') return `Все навыки: уровень ${Number(els.wargmAllSkillLevel && els.wargmAllSkillLevel.value || 4)}. Цель: ${wargmTargetLabel()}.`;
  if (mode === 'attributes') {
    return `Атрибуты: ${Number(els.wargmStrength && els.wargmStrength.value || 0)} / ${Number(els.wargmConstitution && els.wargmConstitution.value || 0)} / ${Number(els.wargmDexterity && els.wargmDexterity.value || 0)} / ${Number(els.wargmIntelligence && els.wargmIntelligence.value || 0)}. Цель: ${wargmTargetLabel()}.`;
  }
  return `Команда: ${(els.wargmCommand && els.wargmCommand.value.trim()) || 'не указана'}. Цель: ${wargmTargetLabel()}.`;
}

function updateWargmModeUi() {
  const mode = els.wargmMode ? els.wargmMode.value : 'item';
  const meta = wargmModeMeta[mode] || wargmModeMeta.item;
  document.querySelectorAll('[data-wargm-mode]').forEach(block => {
    const modes = String(block.dataset.wargmMode || '').split(/\s+/).filter(Boolean);
    block.hidden = !modes.includes(mode);
  });
  if (els.wargmModePill) els.wargmModePill.textContent = meta.label;
  if (els.wargmModeHint) els.wargmModeHint.textContent = meta.hint;
  if (els.wargmPreview) els.wargmPreview.textContent = buildWargmPreview();
  if (els.wargmDeliver) els.wargmDeliver.textContent = mode === 'item' ? 'Выдать предметы' : `Выдать: ${meta.label.toLowerCase()}`;
}

function validateWargmRequest() {
  const mode = els.wargmMode ? els.wargmMode.value : 'item';
  const target = (els.serviceTarget && els.serviceTarget.value || '').trim();
  if (!target) return 'Укажи SteamID или имя игрока в верхнем поле.';
  if (mode === 'item') {
    const hasSingle = !!(els.wargmItemId && els.wargmItemId.value.trim());
    if (!state.wargmItems.length && !hasSingle) return 'Добавь предмет в набор или укажи один ID предмета.';
  }
  if (mode === 'vehicle' && !(els.wargmVehicleId && els.wargmVehicleId.value.trim())) return 'Укажи ID транспорта.';
  if ((mode === 'money' || mode === 'gold' || mode === 'fame') && !Number(els.wargmAmount && els.wargmAmount.value || 0)) return 'Укажи ненулевую сумму.';
  if (mode === 'vip' && Number(els.wargmAmount && els.wargmAmount.value || 0) < 1) return 'Укажи срок VIP в днях.';
  if (mode === 'skill' && !(els.wargmSkill && els.wargmSkill.value.trim())) return 'Укажи навык.';
  if (mode === 'attributes') {
    const values = [els.wargmStrength, els.wargmConstitution, els.wargmDexterity, els.wargmIntelligence].map(input => Number(input && input.value || 0));
    if (values.every(value => value === 0)) return 'Укажи значения атрибутов.';
  }
  if (mode === 'command' && !(els.wargmCommand && els.wargmCommand.value.trim())) return 'Укажи команду.';
  return '';
}

function renderWargmItems() {
  if (!els.wargmItemsList) return;
  els.wargmItemsList.innerHTML = state.wargmItems.length
    ? state.wargmItems.map((item, index) => `<div class="mini-row">
        ${catalogThumbHtml(item.ItemId, 'item', { className: 'wargm-mini-item-thumb', allowGenerated: true })}
        <span><b>${escapeHtml(item.ItemId)}</b>${findCatalogItemLabel(item.ItemId) ? ` <em>${escapeHtml(findCatalogItemLabel(item.ItemId))}</em>` : ''} x ${Number(item.Quantity || 1)}</span>
        <button class="mini-action danger" type="button" data-wargm-remove-item="${index}">Удалить</button>
      </div>`).join('')
    : '<span class="muted-line">Набор пуст. Добавь предметы для кейса или оставь один ID в поле выше.</span>';
  updateWargmModeUi();
}

function addWargmItemFromInputs() {
  const itemId = (els.wargmItemId && els.wargmItemId.value || '').trim();
  const quantity = Math.max(1, Number(els.wargmQty && els.wargmQty.value || 1));
  if (!itemId) {
    toast('Укажи ID предмета.');
    return;
  }
  state.wargmItems.push({ ItemId: itemId, Quantity: quantity });
  if (els.wargmItemId) els.wargmItemId.value = '';
  if (els.wargmQty) els.wargmQty.value = '1';
  renderWargmItems();
}

function resetSelectedPlayerDetailPanels() {
  if (els.playerStatsBody) {
    els.playerStatsBody.innerHTML = '<div class="muted-line">Статистика не читается в фоне. Открой вкладку и нажми обновление, когда она нужна.</div>';
  }
  if (els.playerFactsBody) {
    els.playerFactsBody.innerHTML = '';
  }
  if (els.playerCharacterFactsBody) {
    els.playerCharacterFactsBody.innerHTML = '<div class="muted-line">Данные персонажа не читаются в фоне. Открой вкладку и нажми обновить, когда они нужны.</div>';
  }
  if (els.playerEconomyFactsBody) {
    els.playerEconomyFactsBody.innerHTML = '<div class="muted-line">Баланс не читается в фоне. Нажми "Обновить данные", когда нужно проверить начисление.</div>';
  }
  if (els.playerInventoryBody) {
    els.playerInventoryBody.innerHTML = '<div class="muted-line">Инвентарь не мониторится автоматически. Нажми "Обновить инвентарь", когда он реально нужен.</div>';
  }
}

function getPlayerActionCard() {
  return els.playerActionCard ||
    document.getElementById('playerActionCard') ||
    (els.playerActionModal && els.playerActionModal.querySelector('.modal-card')) ||
    (els.profileActionDock && els.profileActionDock.querySelector('.modal-card'));
}

function mountPlayerActionCard(inline = false) {
  const card = getPlayerActionCard();
  if (!card) return false;
  if (inline && els.profileActionDock) {
    if (card.parentNode !== els.profileActionDock) els.profileActionDock.appendChild(card);
    els.profileActionDock.classList.remove('hidden');
    els.profileActionDock.setAttribute('aria-hidden', 'false');
    if (els.playerActionModal) {
      els.playerActionModal.classList.add('hidden');
      els.playerActionModal.setAttribute('aria-hidden', 'true');
    }
    if (els.playerActionClose) els.playerActionClose.textContent = 'Скрыть';
    return true;
  }
  if (els.playerActionModal && card.parentNode !== els.playerActionModal) els.playerActionModal.appendChild(card);
  if (els.profileActionDock) {
    els.profileActionDock.classList.add('hidden');
    els.profileActionDock.setAttribute('aria-hidden', 'true');
  }
  if (els.playerActionClose) els.playerActionClose.textContent = '×';
  return false;
}

function isPlayerActionDocked() {
  const card = getPlayerActionCard();
  return Boolean(card && els.profileActionDock && card.parentNode === els.profileActionDock);
}

function openPlayerActionModal(steamId, name, runtimeKey = '', tab = 'message', profileId = '', options = {}) {
  if (['message', 'stats', 'inventory', 'items', 'position', 'economy', 'character', 'vehicle', 'guard'].includes(runtimeKey)) {
    tab = runtimeKey;
    runtimeKey = '';
  }
  if (tab === 'stats') tab = 'character';
  const selected = setSelectedPlayerTarget(steamId, name, runtimeKey, profileId);
  const target = selected.target || '';
  els.playerActionTarget.value = target;
  els.playerActionTitle.textContent = `${selected.name || 'Игрок'}${selected.steamId ? ` · ${selected.steamId}` : ''}${selected.profileId ? ` · профиль ${selected.profileId}` : ''}`;
  if (els.playerActionResult) els.playerActionResult.textContent = '';
  resetSelectedPlayerDetailPanels();
  if (els.playerActionVehicleId && !els.playerActionVehicleId.value) {
    const firstVehicle = (state.vehicles || [])[0];
    els.playerActionVehicleId.value = firstVehicle ? catalogIdFor(firstVehicle, 'vehicle') : 'BPC_WolfsWagen';
  }
  setPlayerActionTab(tab);
  updatePlayerWelcomeControls();
  updatePlayerBattlepassControls();
  refreshWelcomeTimers().catch(() => updatePlayerWelcomeControls());
  const shouldDock = Boolean(options.inline);
  if (mountPlayerActionCard(shouldDock)) return;
  els.playerActionModal.classList.remove('hidden');
  els.playerActionModal.setAttribute('aria-hidden', 'false');
}

function closePlayerActionModal() {
  if (isPlayerActionDocked()) {
    if (els.profileActionDock) {
      els.profileActionDock.classList.add('hidden');
      els.profileActionDock.setAttribute('aria-hidden', 'true');
    }
    return;
  }
  els.playerActionModal.classList.add('hidden');
  els.playerActionModal.setAttribute('aria-hidden', 'true');
}

function setPlayerActionTab(tab) {
  document.querySelectorAll('[data-player-tab]').forEach(btn => btn.classList.toggle('active', btn.dataset.playerTab === tab));
  document.querySelectorAll('[data-player-pane]').forEach(pane => {
    pane.classList.toggle('active', pane.dataset.playerPane === tab);
    pane.classList.remove('group-active');
  });
  if (tab === 'vehicle' && els.playerActionVehicleId && !els.playerActionVehicleId.value) {
    const firstVehicle = (state.vehicles || [])[0];
    els.playerActionVehicleId.value = firstVehicle ? catalogIdFor(firstVehicle, 'vehicle') : 'BPC_WolfsWagen';
  }
  if (tab === 'stats' && state.selectedPlayerTarget) {
    loadPlayerStats(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey).catch(toast);
  }
  if (tab === 'character' && state.selectedPlayerTarget) {
    loadPlayerFacts(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey).catch(toast);
  }
  if (tab === 'economy' && state.selectedPlayerTarget) {
    loadPlayerFacts(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey).catch(toast);
  }
  if (tab === 'inventory' && state.selectedPlayerTarget) {
    loadPlayerInventory(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey).catch(toast);
  }
}

function modalTargetBody() {
  const target = (els.playerActionTarget && els.playerActionTarget.value || '').trim();
  const selected = state.selectedPlayerTarget;
  if (selected && (target === selected.target || target === selected.steamId || target === selected.name)) {
    return selectedPlayerBody();
  }
  return playerTargetBody(target);
}

function uniqueInventoryEntityIds(rows) {
  const seen = new Set();
  return (Array.isArray(rows) ? rows : [])
    .filter(row => row && row.entityId)
    .sort((a, b) => Number(b.depth || 0) - Number(a.depth || 0))
    .map(row => String(row.entityId || '').trim())
    .filter(id => {
      if (!id || id === '0' || seen.has(id)) return false;
      seen.add(id);
      return true;
    });
}

async function destroyInventoryEntitiesFast(ids, target) {
  const cleanIds = uniqueInventoryEntityIds((ids || []).map(id => ({ entityId: id })));
  if (!cleanIds.length) throw new Error('Нет EntityID для удаления.');
  const chunks = [];
  for (let i = 0; i < cleanIds.length; i += 6) chunks.push(cleanIds.slice(i, i + 6));
  const result = { ok: true, requested: cleanIds.length, batch: true, chunks: chunks.length, queued: 0, failed: [] };
  for (let i = 0; i < chunks.length; i += 1) {
    const chunk = chunks[i];
    try {
      const body = Object.assign({}, target, {
        targetSteamId: target.steamId,
        targetName: target.name,
        targetRuntimeKey: target.runtimeKey,
        entityIds: chunk,
        entitiesText: chunk.join(' ')
      });
      const batch = await api('/api/player/destroy-inventory-entities', {
        method: 'POST',
        body: JSON.stringify(body)
      });
      const batchData = batch && (batch.payload || batch.data || batch);
      const payload = batchData && (batchData.payload || batchData.data || batchData);
      result.queued += Number((payload && (payload.batchSize || payload.requested || payload.sent)) || chunk.length || 0);
    } catch (err) {
      result.ok = false;
      result.failed.push(`batch ${i + 1}: ${err && err.message ? err.message : String(err)}`);
    }
    if (i < chunks.length - 1) await sleep(220);
  }
  return result;
}

async function addItemToInventory(itemId, quantity, target) {
  const cleanItemId = String(itemId || '').trim();
  const cleanQuantity = Math.max(1, Math.min(Number(quantity || 1), 20));
  if (!cleanItemId) throw new Error('ID предмета не указан.');
  return api('/api/plugin-command', {
    method: 'POST',
    body: JSON.stringify({
      command: 'add_inventory_item',
      args: Object.assign({}, target, {
        targetSteamId: target.steamId,
        targetName: target.name,
        targetRuntimeKey: target.runtimeKey,
        itemId: cleanItemId,
        quantity: cleanQuantity
      })
    })
  });
}

async function grantAllSkillsToSelectedPlayer() {
  const level = Math.max(0, Math.min(4, Number(els.playerActionSkillLevel && els.playerActionSkillLevel.value || 4)));
  const experience = 0;
  const target = modalTargetBody();
  const skills = mergeSkillCatalog(state.skillCatalog || FULL_SCUM_SKILLS)
    .map(skill => String(skill.skill || skill.key || skill.name || '').trim())
    .filter(Boolean)
    .filter(skill => !isAllSkillsValue(skill))
    .filter((skill, index, list) => list.findIndex(item => item.toLowerCase() === skill.toLowerCase()) === index);

  if (!skills.length) throw new Error('Каталог навыков пуст.');
  if (!window.confirm(`Поставить ${skills.length} навыка(ов) в безопасную очередь уровнем ${level}?`)) {
    return { ok: false, cancelled: true };
  }

  if (els.playerActionResult) {
    showResult(els.playerActionResult, `Ставлю ${skills.length} навыка(ов) в безопасную очередь. Сервер выдаст их по одному.`);
  }
  const response = await api('/api/player/set-skill', {
    method: 'POST',
    body: JSON.stringify(Object.assign({}, target, { skill: 'allskills', skillName: 'allskills', allSkills: true, AllSkills: true, level, experience }))
  });
  refreshSelectedFactsSoon(1500);
  return Object.assign({ ok: true, queued: true, level, total: skills.length }, response || {});
}

const SUPPORT_AUTHOR_CARD = '2202 2068 7570 5381';
const SUPPORT_AUTHOR_CARD_COMPACT = SUPPORT_AUTHOR_CARD.replace(/\s+/g, '');
const SUPPORT_AUTHOR_BANK_URL = 'https://online.sberbank.ru/app';

async function copySupportAuthorCard(showToast = true) {
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(SUPPORT_AUTHOR_CARD_COMPACT);
    } else {
      const ta = document.createElement('textarea');
      ta.value = SUPPORT_AUTHOR_CARD_COMPACT;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.left = '-9999px';
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      ta.remove();
    }
    if (showToast) toast('Номер карты Сбербанка скопирован.');
    return true;
  } catch (_) {
    if (showToast) toast(`Карта Сбербанк: ${SUPPORT_AUTHOR_CARD}`);
    return false;
  }
}

function openSupportAuthorBank() {
  const opened = window.open(SUPPORT_AUTHOR_BANK_URL, '_blank', 'noopener,noreferrer');
  if (!opened) toast('Откройте Сбербанк Онлайн и вставьте номер карты из окна поддержки.');
}

function closeSupportAuthorDialog() {
  document.querySelector('[data-support-author-overlay]')?.remove();
}

function showSupportAuthorDialog() {
  closeSupportAuthorDialog();
  const holder = document.createElement('div');
  holder.innerHTML = `<div class="support-author-modal wargm-rule-modal" data-support-author-overlay="true" role="dialog" aria-modal="true">
    <div class="support-author-card wargm-rule-modal-card">
      <div class="wargm-rule-modal-head">
        <div>
          <span class="wargm-modal-kicker">Поддержка автора</span>
          <h3>Перевод по карте Сбербанка</h3>
          <p>Номер карты можно скопировать и вставить в Сбербанк Онлайн.</p>
        </div>
        <div class="wargm-rule-modal-actions">
          <button type="button" class="mini-action" data-support-author-close="true">Закрыть</button>
        </div>
      </div>
      <div class="support-author-body">
        <div class="support-author-bank">Сбербанк</div>
        <button type="button" class="support-author-number" data-support-author-copy="true" title="Скопировать номер карты">${escapeHtml(SUPPORT_AUTHOR_CARD)}</button>
        <div class="support-author-hint">Панель копирует номер карты и открывает официальный Сбербанк Онлайн. В банке выберите перевод по номеру карты и вставьте номер.</div>
        <div class="support-author-actions">
          <a class="btn primary" href="${escapeAttr(SUPPORT_AUTHOR_BANK_URL)}" target="_blank" rel="noopener noreferrer" data-support-author-open-bank="true">Открыть Сбербанк Онлайн</a>
          <button type="button" class="btn" data-support-author-copy="true">Копировать карту</button>
        </div>
      </div>
    </div>
  </div>`;
  const modal = holder.firstElementChild;
  document.body.appendChild(modal);
  modal.addEventListener('click', evt => {
    if (evt.target.closest('[data-support-author-close]')) {
      closeSupportAuthorDialog();
      return;
    }
    if (evt.target.closest('[data-support-author-copy]')) {
      copySupportAuthorCard(true);
      return;
    }
    if (evt.target.closest('[data-support-author-open-bank]')) {
      copySupportAuthorCard(false);
    }
  });
  copySupportAuthorCard(false);
  openSupportAuthorBank();
}

document.querySelectorAll('.nav-item').forEach(btn => btn.addEventListener('click', () => setPage(btn.dataset.page)));
document.querySelectorAll('[data-event-tab]').forEach(btn => btn.addEventListener('click', () => {
  state.eventTab = btn.dataset.eventTab;
  document.querySelectorAll('[data-event-tab]').forEach(tab => tab.classList.toggle('active', tab === btn));
  refreshEvents().catch(toast);
}));
document.querySelectorAll('[data-service-tab]').forEach(btn => btn.addEventListener('click', () => setServiceTab(btn.dataset.serviceTab)));
document.querySelectorAll('[data-economy-tab]').forEach(btn => btn.addEventListener('click', () => setEconomyTab(btn.dataset.economyTab)));

els.apiKey.value = state.apiKey;
if (els.serverHost) els.serverHost.value = state.apiBase || window.location.origin;
setPanelLanguage(state.language);
if (els.panelLanguage) {
  els.panelLanguage.addEventListener('change', () => {
    setPanelLanguage(els.panelLanguage.value);
    setPage(state.page, false);
    refreshStatus().catch(toast);
    if (state.page === 'players') renderPlayers();
  });
}
els.saveApiKey.addEventListener('click', () => {
  state.apiKey = els.apiKey.value.trim();
  if (els.serverHost) {
    const rawBase = normalizeApiBase(els.serverHost.value);
    state.apiBase = rawBase && rawBase !== window.location.origin ? rawBase : '';
    localStorage.setItem('nedjin.apiBase.v2', state.apiBase);
  }
  localStorage.setItem('nedjin.apiKey.v2', state.apiKey);
  localStorage.setItem('nedjin.apiKey', state.apiKey);
  showResult(els.quickResult, 'Подключено. Обновляю состояние...');
  refreshCatalogs().catch(toast);
  refreshCurrent();
});
if (els.supportAuthorBtn) els.supportAuthorBtn.addEventListener('click', showSupportAuthorDialog);
if (els.downloadDiagnosticsPackage) els.downloadDiagnosticsPackage.addEventListener('click', () => downloadDiagnosticsPackage().catch(toast));
if (els.serverList) els.serverList.addEventListener('click', evt => {
  const btn = evt.target.closest('[data-server-action]');
  if (!btn) return;
  setPage(btn.dataset.serverAction);
});
document.querySelectorAll('[data-server-control-action]').forEach(btn => {
  btn.addEventListener('click', () => runServerControlAction(btn.dataset.serverControlAction).catch(toast));
});
if (els.serverConfigList) els.serverConfigList.addEventListener('click', evt => {
  const btn = evt.target.closest('[data-server-config]');
  if (!btn) return;
  loadServerConfig(btn.dataset.serverConfig).catch(toast);
});
if (els.serverConfigRefresh) els.serverConfigRefresh.addEventListener('click', () => refreshServerConfigs().catch(toast));
if (els.serverConfigExpand) els.serverConfigExpand.addEventListener('click', toggleServerConfigExpanded);
if (els.serverConfigReload) els.serverConfigReload.addEventListener('click', () => loadServerConfig(state.selectedServerConfig).catch(toast));
if (els.serverConfigSave) els.serverConfigSave.addEventListener('click', () => saveServerConfig().catch(toast));
if (els.serverConfigContent) els.serverConfigContent.addEventListener('input', () => {
  queueServerConfigResize();
  updateServerConfigSaveState();
});
window.addEventListener('resize', queueServerConfigResize);
document.addEventListener('keydown', evt => {
  if (evt.key === 'Escape' && state.serverConfigExpanded) {
    setServerConfigExpanded(false);
  }
});
els.refreshBtn.addEventListener('click', refreshCurrent);
els.refreshEventsSmall.addEventListener('click', () => refreshEvents().catch(toast));
els.refreshPlayers.addEventListener('click', () => refreshPlayers().catch(toast));
els.playerSearch.addEventListener('input', renderPlayers);
if (els.playersSort) els.playersSort.addEventListener('change', renderPlayers);
els.refreshSquads.addEventListener('click', () => refreshSquads().catch(toast));
els.squadSearch.addEventListener('input', renderSquads);
if (els.squadsList) els.squadsList.addEventListener('click', evt => {
  const kick = evt.target.closest('[data-squad-kick]');
  if (kick) {
    evt.preventDefault();
    evt.stopPropagation();
    const memberId = Number(kick.dataset.memberId || 0);
    const squadId = Number(kick.dataset.squadId || 0);
    const steamId = kick.dataset.steam || '';
    const name = kick.dataset.name || '';
    const label = name || steamId || 'участника';
    if (!confirmDanger(`Исключить ${label} из отряда?`)) return;
    showActionResult(els.quickResult, () => api('/api/squads/kick', {
      method: 'POST',
      body: JSON.stringify({ memberId, squadId, steamId, name, squadName: kick.dataset.squadName || '' })
    })).then(() => refreshSquads().catch(toast)).catch(toast);
    return;
  }
  const squadOpen = evt.target.closest('[data-squad-open]');
  if (squadOpen) {
    evt.preventDefault();
    const key = squadOpen.dataset.squadOpen || '';
    state.selectedSquadKey = state.selectedSquadKey === key ? '' : key;
    renderSquads();
    return;
  }
  const member = evt.target.closest('[data-squad-member]');
  if (!member) return;
  const steamId = member.dataset.steam || '';
  const name = member.dataset.name || '';
  const player = findPlayerByIdentity(steamId, name, '') || { steamId, name };
  setPage('players', false);
  openPlayerProfile(player);
  refreshPlayers().catch(() => {});
});
els.refreshChat.addEventListener('click', () => refreshChat().catch(toast));
els.chatSearch.addEventListener('input', renderChat);
els.chatChannel.addEventListener('change', () => {
  renderChat();
  refreshChatAuxChannel(els.chatChannel.value, false).then(renderChat).catch(toast);
});
if (els.chatList) els.chatList.addEventListener('click', async evt => {
  const action = evt.target.closest('[data-chat-act]');
  if (!action) return;
  evt.preventDefault();
  evt.stopPropagation();
  const row = action.closest('.chat-message');
  const steamId = action.dataset.steam || (row && row.dataset.steam) || '';
  const name = action.dataset.name || (row && row.dataset.name) || '';
  if (action.dataset.chatAct === 'copy-id') {
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(steamId);
      } else {
        const ta = document.createElement('textarea');
        ta.value = steamId;
        ta.setAttribute('readonly', '');
        ta.style.position = 'fixed';
        ta.style.left = '-9999px';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        ta.remove();
      }
      toast(steamId ? `SteamID скопирован: ${steamId}` : 'SteamID не найден');
    } catch (_) {
      toast(steamId);
    }
    return;
  }
  if (action.dataset.chatAct === 'profile') {
    const player = findPlayerByIdentity(steamId, name, '') || { steamId, name };
    setPage('players', false);
    openPlayerProfile(player);
    refreshPlayers().catch(() => {});
  }
});
if (els.clearChat) els.clearChat.addEventListener('click', async () => {
  const channel = els.chatChannel ? els.chatChannel.value : 'global';
  const target = logClearTargetForChatChannel(channel);
  const label = target === 'kills' ? 'логи убийств' : target === 'action-log' ? 'журнал NeDjin' : 'чат';
  if (!window.confirm(`Очистить ${label} в файлах проекта? SCUM.log не изменится.`)) return;
  try {
    showResult(els.chatResult, 'Очищаю журнал проекта...');
    await clearProjectLogs(target);
    resetLocalLogState(target);
    state.chatAuxLoading = {};
    if (els.chatSearch) els.chatSearch.value = '';
    renderChat();
    showResult(els.chatResult, 'Журналы проекта очищены. SCUM.log не изменялся.');
  } catch (err) {
    showResult(els.chatResult, { ok: false, error: err && err.message ? err.message : String(err) });
  }
});
els.refreshKills.addEventListener('click', () => refreshKills().catch(toast));
if (els.clearKills) els.clearKills.addEventListener('click', async () => {
  if (!window.confirm('Очистить логи убийств в файлах проекта? SCUM.log не изменится.')) return;
  try {
    await clearProjectLogs('kills');
    resetLocalLogState('kills');
    renderKills();
    toast('Журналы проекта очищены. SCUM.log не изменялся.');
  } catch (err) {
    toast(err);
  }
});
els.killsSearch.addEventListener('input', renderKills);
if (els.killsList) els.killsList.addEventListener('click', evt => {
  const toggle = evt.target.closest('[data-kill-open]');
  if (!toggle) return;
  evt.preventDefault();
  const key = toggle.dataset.killOpen || '';
  state.selectedKillKey = state.selectedKillKey === key ? '' : key;
  renderKills();
});
els.refreshEconomy.addEventListener('click', () => refreshEconomy().catch(toast));
els.refreshDownloads.addEventListener('click', () => refreshDownloads().catch(toast));
els.homeListRefresh.addEventListener('click', () => refreshHomes().catch(toast));
if (els.refreshMap) els.refreshMap.addEventListener('click', () => refreshMap().catch(toast));
els.layerPlayers.addEventListener('change', renderMap);
els.layerVehicles.addEventListener('change', renderMap);
if (els.layerChests) els.layerChests.addEventListener('change', renderMap);
els.layerFlags.addEventListener('change', renderMap);
if (els.mapZoomIn) els.mapZoomIn.addEventListener('click', () => { state.mapZoom += 0.2; applyMapZoom(); });
if (els.mapZoomOut) els.mapZoomOut.addEventListener('click', () => { state.mapZoom -= 0.2; applyMapZoom(); });
if (els.mapReset) els.mapReset.addEventListener('click', () => { state.mapZoom = 1; applyMapZoom(); setMapSelection(null); });
if (els.mapImage) els.mapImage.addEventListener('click', evt => {
  if (!state.map || !els.mapSelection) return;
  const rect = els.mapImage.getBoundingClientRect();
  let px = (evt.clientX - rect.left) / rect.width;
  let py = (evt.clientY - rect.top) / rect.height;
  const bounds = state.map.bounds || {};
  const minX = Number(bounds.minX ?? -768000);
  const maxX = Number(bounds.maxX ?? 768000);
  const minY = Number(bounds.minY ?? -768000);
  const maxY = Number(bounds.maxY ?? 768000);
  if (bounds.invertX ?? true) px = 1 - px;
  if (bounds.invertY ?? true) py = 1 - py;
  const worldX = minX + (maxX - minX) * px;
  const worldY = minY + (maxY - minY) * py;
  const x = bounds.swapAxes ? worldY : worldX;
  const y = bounds.swapAxes ? worldX : worldY;
  setMapSelection({ kind: 'точка', title: 'Выбранная точка', x, y, z: 0 });
});
if (els.mapMarkers) els.mapMarkers.addEventListener('click', evt => {
  const marker = evt.target.closest('[data-map-marker]');
  if (!marker) return;
  setMapSelection({
    kind: marker.dataset.kind || '',
    title: marker.dataset.title || marker.dataset.name || '',
    steamId: marker.dataset.steam || '',
    name: marker.dataset.name || '',
    runtimeKey: marker.dataset.runtime || '',
    x: Number(marker.dataset.x || 0),
    y: Number(marker.dataset.y || 0),
    z: Number(marker.dataset.z || 0)
  });
});
if (els.mapSelection) els.mapSelection.addEventListener('click', async evt => {
  const action = evt.target.closest('[data-map-selection-action]');
  if (!action || !state.mapSelection) return;
  if (action.dataset.mapSelectionAction === 'use-coords') {
    applySelectedMapCoords();
    toast('Координаты вставлены в поля телепорта.');
  } else if (action.dataset.mapSelectionAction === 'copy-coords') {
    const s = state.mapSelection;
    const text = `${Number(s.x || 0).toFixed(0)} ${Number(s.y || 0).toFixed(0)} ${Number(s.z || 0).toFixed(0)}`;
    if (navigator.clipboard) await navigator.clipboard.writeText(text);
    toast('Координаты скопированы.');
  } else if (action.dataset.mapSelectionAction === 'open-player') {
    const s = state.mapSelection;
    openPlayerActionModal(s.steamId || '', s.name || '', s.runtimeKey || '', 'position');
    applySelectedMapCoords();
  }
});
els.refreshTrace.addEventListener('click', () => refreshDiagnostics().catch(toast));
if (els.refreshQueueStatus) els.refreshQueueStatus.addEventListener('click', () => refreshDiagnostics().catch(toast));
els.refreshServerLog.addEventListener('click', () => refreshDiagnostics().catch(toast));
if (els.refreshRuntimeLog) els.refreshRuntimeLog.addEventListener('click', () => refreshDiagnostics().catch(toast));
if (els.refreshActionLog) els.refreshActionLog.addEventListener('click', () => refreshDiagnostics().catch(toast));
if (els.smokeBridge) els.smokeBridge.addEventListener('click', async () => {
  await showActionResult(els.quickResult, async () => {
    const status = await api('/api/status');
    await refreshStatus().catch(() => {});
    const bridge = status && status.bridge ? status.bridge : {};
    return {
      ok: !!bridge.online,
      message: bridge.online
        ? `Мост в сети${bridge.heartbeatAgeSeconds == null ? '' : `, heartbeat ${bridge.heartbeatAgeSeconds}s`}.`
        : `Мост не в сети${bridge.message ? `: ${bridge.message}` : '.'}`,
      bridge
    };
  });
});
if (els.smokeApple) els.smokeApple.addEventListener('click', async () => {
  await showActionResult(els.quickResult, () => api('/api/player/grant-item', {
    method: 'POST',
    body: JSON.stringify(smokeTargetBody({ itemId: 'Apple_2', quantity: 1 }))
  }));
});
if (els.smokeWeapon) els.smokeWeapon.addEventListener('click', async () => {
  await showActionResult(els.quickResult, () => api('/api/player/grant-item', {
    method: 'POST',
    body: JSON.stringify(smokeTargetBody({ itemId: 'Weapon_MK18', quantity: 1 }))
  }));
});
if (els.smokeVehicle) els.smokeVehicle.addEventListener('click', async () => {
  const vehicleId = (els.rentalVehicle && els.rentalVehicle.value) || 'BPC_WolfsWagen';
  await showActionResult(els.quickResult, () => api('/api/player/spawn-vehicle', {
    method: 'POST',
    body: JSON.stringify(smokeTargetBody({ vehicleId }))
  }));
});

els.quickBroadcast.addEventListener('click', async () => {
  await showActionResult(els.quickResult, () => api('/api/chat', { method: 'POST', body: JSON.stringify({ message: scumChatSafeMessage(els.quickMessage.value), channel: 'server' }) }));
  await sleep(750);
  await refreshEvents().catch(toast);
});
els.quickCommandRun.addEventListener('click', async () => {
  await showActionResult(els.quickResult, () => api('/api/rcon', { method: 'POST', body: JSON.stringify({ command: els.quickCommand.value }) }));
  await sleep(750);
  await refreshEvents().catch(toast);
});
els.welcomeClaim.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, async () => {
    const result = await api('/api/welcome-pack/claim', { method: 'POST', body: JSON.stringify(serviceTargetBody()) });
    await refreshWelcomeTimers();
    return result;
  });
});
els.welcomeReset.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, async () => {
    const result = await api('/api/welcome-pack/reset-timer', { method: 'POST', body: JSON.stringify(serviceTargetBody()) });
    await refreshWelcomeTimers();
    return result;
  });
});
if (els.welcomeTimers) els.welcomeTimers.addEventListener('click', async evt => {
  const btn = evt.target.closest('[data-welcome-reset]');
  if (!btn) return;
  const label = btn.dataset.welcomeSteam || btn.dataset.welcomeName || btn.dataset.welcomeReset || 'игрок';
  if (!confirmDanger(`Сбросить стартовый набор для ${label}? Игрок сможет получить его заново.`)) return;
  await showActionResult(els.serviceResult, async () => {
    const result = await api('/api/welcome-pack/reset-timer', {
      method: 'POST',
      body: JSON.stringify({
        steamId: btn.dataset.welcomeSteam || null,
        name: btn.dataset.welcomeName || null,
        targetSteamId: btn.dataset.welcomeSteam || null,
        targetName: btn.dataset.welcomeName || null
      })
    });
    await refreshWelcomeTimers();
    return result;
  });
});
els.homeSet.addEventListener('click', async () => {
  const body = Object.assign(serviceTargetBody(), { label: els.homeLabel.value.trim() || 'home' });
  await showActionResult(els.serviceResult, async () => {
    const result = await api('/api/home/set', { method: 'POST', body: JSON.stringify(body) });
    await refreshHomes();
    return result;
  });
});
els.homeList.addEventListener('click', async evt => {
  const btn = evt.target.closest('button[data-home-x]');
  if (!btn) return;
  const body = Object.assign(serviceTargetBody(), {
    label: btn.dataset.homeLabel || '',
    x: Number(btn.dataset.homeX || 0),
    y: Number(btn.dataset.homeY || 0),
    z: Number(btn.dataset.homeZ || 0)
  });
  await showActionResult(els.serviceResult, () => api('/api/home/teleport', { method: 'POST', body: JSON.stringify(body) }));
});
els.fastRoute.addEventListener('change', () => {
  const opt = els.fastRoute.selectedOptions[0];
  const parts = opt && opt.dataset.point ? opt.dataset.point.split(',') : [];
  if (parts.length >= 3) {
    els.fastX.value = parts[0];
    els.fastY.value = parts[1];
    els.fastZ.value = parts[2];
  }
  if (els.fastFare && opt && opt.dataset.price !== undefined && opt.dataset.price !== '') {
    els.fastFare.value = opt.dataset.price;
  }
  updateFastTravelPricePreview();
});
[
  els.serviceTarget,
  els.fastFare,
  els.fastX,
  els.fastY,
  els.fastZ
].forEach(input => {
  if (input) input.addEventListener('input', updateFastTravelPricePreview);
});
els.fastTravelGo.addEventListener('click', async () => {
  const body = Object.assign(serviceTargetBody(), {
    alias: els.fastRoute.value,
    x: Number(els.fastX.value || 0),
    y: Number(els.fastY.value || 0),
    z: Number(els.fastZ.value || 0)
  });
  const fareText = els.fastFare ? els.fastFare.value.trim() : '';
  if (fareText !== '') {
    body.amount = Number(fareText || 0);
    body.fare = Number(fareText || 0);
  }
  await showActionResult(els.serviceResult, () => api('/api/fast-travel/go', { method: 'POST', body: JSON.stringify(body) }));
});
if (els.rentalVehicle) els.rentalVehicle.addEventListener('change', applyRentalVehicleDefaults);
els.rentVehicle.addEventListener('click', async () => {
  const opt = els.rentalVehicle.selectedOptions[0];
  const body = Object.assign(serviceTargetBody(), {
    vehicleId: els.rentalVehicle.value,
    alias: opt ? opt.dataset.alias : '',
    minutes: Number(els.rentalMinutes && els.rentalMinutes.value || 10)
  });
  await showActionResult(els.serviceResult, () => api('/api/vehicle-rental/rent', { method: 'POST', body: JSON.stringify(body) }));
});
if (els.rentalCleanup) els.rentalCleanup.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, () => api('/api/vehicle-rental/cleanup', { method: 'POST', body: JSON.stringify({}) }));
});
els.scanSector.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, () => api('/api/sector-scan', { method: 'POST', body: JSON.stringify(serviceTargetBody()) }));
});
if (els.wargmSync) els.wargmSync.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, () => api('/api/wargm/sync', { method: 'POST', body: JSON.stringify({}) }));
});
if (els.wargmConfirm) els.wargmConfirm.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, () => api('/api/wargm/confirm-delivered', { method: 'POST', body: JSON.stringify({}) }));
});
if (els.discordStatus) els.discordStatus.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, () => api('/api/discord/status'));
});
if (els.discordSaveToken) els.discordSaveToken.addEventListener('click', async () => {
  const token = els.discordBotToken ? els.discordBotToken.value.trim() : '';
  await showActionResult(els.serviceResult, async () => {
    if (!token) throw new Error('Вставь Bot token перед сохранением.');
    const data = await api('/api/discord-log-bridge/secret', {
      method: 'POST',
      body: JSON.stringify({ botToken: token })
    });
    if (els.discordBotToken) els.discordBotToken.value = '';
    return data;
  });
});
if (els.discordTest) els.discordTest.addEventListener('click', async () => {
  await showActionResult(els.serviceResult, () => api('/api/discord/test', {
    method: 'POST',
    body: JSON.stringify({
      route: els.discordRoute ? els.discordRoute.value : 'system',
      message: els.discordMessage ? els.discordMessage.value : ''
    })
  }));
});
if (els.wargmAddItem) els.wargmAddItem.addEventListener('click', addWargmItemFromInputs);
if (els.wargmClearItems) els.wargmClearItems.addEventListener('click', () => {
  state.wargmItems = [];
  renderWargmItems();
});
if (els.wargmMode) els.wargmMode.addEventListener('change', updateWargmModeUi);
[
  els.serviceTarget,
  els.wargmItemId,
  els.wargmQty,
  els.wargmVehicleId,
  els.wargmAmount,
    els.wargmSkill,
    els.wargmSkillLevel,
    els.wargmSkillExperience,
    els.wargmAllSkillLevel,
    els.wargmAllSkillExperience,
    els.wargmStrength,
  els.wargmConstitution,
  els.wargmDexterity,
  els.wargmIntelligence,
  els.wargmCommand
].forEach(input => {
  if (input) input.addEventListener('input', updateWargmModeUi);
});
if (els.wargmItemsList) els.wargmItemsList.addEventListener('click', evt => {
  const btn = evt.target.closest('[data-wargm-remove-item]');
  if (!btn) return;
  state.wargmItems.splice(Number(btn.dataset.wargmRemoveItem), 1);
  renderWargmItems();
});
els.wargmDeliver.addEventListener('click', async () => {
  const validationMessage = validateWargmRequest();
  if (validationMessage) {
    toast(validationMessage);
    if (els.wargmPreview) els.wargmPreview.textContent = validationMessage;
    return;
  }
  const selectedMode = els.wargmMode.value;
  const amount = Number(els.wargmAmount.value || 0);
  const items = state.wargmItems.length
    ? state.wargmItems
    : (els.wargmItemId.value.trim() ? [{ ItemId: els.wargmItemId.value.trim(), Quantity: Number(els.wargmQty.value || 1) }] : []);
  const isAllSkills = selectedMode === 'allskills';
  const skillLevel = isAllSkills
    ? Number(els.wargmAllSkillLevel && els.wargmAllSkillLevel.value || 4)
    : Number(els.wargmSkillLevel && els.wargmSkillLevel.value || 0);
  const skillExperience = isAllSkills
    ? Number(els.wargmAllSkillExperience && els.wargmAllSkillExperience.value || 0)
    : Number(els.wargmSkillExperience && els.wargmSkillExperience.value || 0);
  const body = Object.assign(serviceTargetBody(), {
    mode: selectedMode,
    itemId: els.wargmItemId.value.trim(),
    quantity: Number(els.wargmQty.value || 1),
    Items: items,
    vehicleId: els.wargmVehicleId.value.trim(),
    amount,
    durationDays: selectedMode === 'vip' ? Math.max(1, Number(amount || 30)) : undefined,
    currency: selectedMode === 'gold' ? 'Gold' : 'Normal',
    skill: isAllSkills ? 'allskills' : (els.wargmSkill ? els.wargmSkill.value.trim() : ''),
    skillName: isAllSkills ? 'allskills' : (els.wargmSkill ? els.wargmSkill.value.trim() : ''),
    allSkills: isAllSkills,
    AllSkills: isAllSkills,
    level: skillLevel,
    skillLevel,
    experience: skillExperience,
    skillExperience,
    strength: Number(els.wargmStrength && els.wargmStrength.value || 0),
    constitution: Number(els.wargmConstitution && els.wargmConstitution.value || 0),
    dexterity: Number(els.wargmDexterity && els.wargmDexterity.value || 0),
    intelligence: Number(els.wargmIntelligence && els.wargmIntelligence.value || 0),
    command: els.wargmCommand ? els.wargmCommand.value.trim() : ''
  });
  await showActionResult(els.serviceResult, () => api('/api/wargm/manual-deliver', { method: 'POST', body: JSON.stringify(body) }));
});
els.sendChat.addEventListener('click', async () => {
  const target = playerTargetBody(els.chatTarget.value.trim());
  const body = { channel: 'server', message: scumChatSafeMessage(els.chatMessage.value) };
  if (target.steamId || target.name || target.runtimeKey) {
    Object.assign(body, target, {
      targetSteamId: target.steamId,
      targetName: target.name,
      targetRuntimeKey: target.runtimeKey
    });
  }
  await showActionResult(els.chatResult, () => api('/api/chat', { method: 'POST', body: JSON.stringify(body) }));
});
els.economyApply.addEventListener('click', async () => {
  const body = Object.assign(playerTargetBody(els.economyTarget.value.trim()), {
    currency: els.economyCurrency.value,
    amount: Number(els.economyAmount.value || 0)
  });
  await showActionResult(els.economyResult, () => api('/api/player/change-money', { method: 'POST', body: JSON.stringify(body) }));
});
els.fameApply.addEventListener('click', async () => {
  const body = Object.assign(playerTargetBody(els.economyTarget.value.trim()), { amount: Number(els.fameAmount.value || 0) });
  await showActionResult(els.economyResult, () => api('/api/player/change-fame', { method: 'POST', body: JSON.stringify(body) }));
});
els.runConsole.addEventListener('click', async () => {
  await showActionResult(els.consoleResult, () => api('/api/rcon', { method: 'POST', body: JSON.stringify({ command: els.consoleCommand.value }) }));
});
els.grantItem.addEventListener('click', async () => {
  const target = els.itemTarget.value.trim();
  const body = Object.assign(playerTargetBody(target), { itemId: els.itemId.value.trim(), quantity: Number(els.itemQty.value || 1) });
  await showActionResult(els.consoleResult, () => api('/api/player/grant-item', { method: 'POST', body: JSON.stringify(body) }));
});
els.equipItem.addEventListener('click', async () => {
  const target = els.itemTarget.value.trim();
  const body = Object.assign(playerTargetBody(target), { itemId: els.itemId.value.trim(), quantity: Number(els.itemQty.value || 1), equip: true });
  await showActionResult(els.consoleResult, () => api('/api/player/equip-item', { method: 'POST', body: JSON.stringify(body) }));
});
els.prewarmItem.addEventListener('click', async () => {
  const body = { itemId: els.itemId.value.trim(), quantity: Number(els.itemQty.value || 1) };
  await showActionResult(els.consoleResult, () => api('/api/items/prewarm', { method: 'POST', body: JSON.stringify(body) }));
});
els.addItemBatch.addEventListener('click', () => {
  const itemId = els.itemId.value.trim();
  if (!itemId) return;
  state.itemBatch.push({ itemId, quantity: Number(els.itemQty.value || 1) });
  renderItemBatch();
});
els.grantItemBatch.addEventListener('click', async () => {
  const target = els.itemTarget.value.trim();
  const body = Object.assign(playerTargetBody(target), { Items: state.itemBatch.map(item => ({ ItemId: item.itemId, Quantity: item.quantity })) });
  await showActionResult(els.consoleResult, async () => {
    const result = await api('/api/player/item-batch', { method: 'POST', body: JSON.stringify(body) });
    state.itemBatch = [];
    renderItemBatch();
    return result;
  });
});
els.itemBatchList.addEventListener('click', evt => {
  const btn = evt.target.closest('[data-batch-remove]');
  if (!btn) return;
  state.itemBatch.splice(Number(btn.dataset.batchRemove), 1);
  renderItemBatch();
});
els.teleportPlayer.addEventListener('click', async () => {
  const body = Object.assign(playerTargetBody(els.actionTarget.value.trim()), {
    x: Number(els.teleportX.value || 0),
    y: Number(els.teleportY.value || 0),
    z: Number(els.teleportZ.value || 0)
  });
  await showActionResult(els.consoleResult, () => api('/api/player/teleport', { method: 'POST', body: JSON.stringify(body) }));
});
els.changeMoney.addEventListener('click', async () => {
  const body = Object.assign(playerTargetBody(els.actionTarget.value.trim()), { currency: els.moneyCurrency.value, amount: Number(els.moneyAmount.value || 0) });
  await showActionResult(els.consoleResult, () => api('/api/player/change-money', { method: 'POST', body: JSON.stringify(body) }));
});
els.spawnVehicle.addEventListener('click', async () => {
  const body = Object.assign(playerTargetBody(els.actionTarget.value.trim()), {
    vehicleId: els.vehicleId.value.trim(),
    minutes: Number(els.vehicleMinutes && els.vehicleMinutes.value || 0)
  });
  await showActionResult(els.consoleResult, () => api('/api/player/spawn-vehicle', { method: 'POST', body: JSON.stringify(body) }));
});
els.playersList.addEventListener('click', async evt => {
  const btn = evt.target.closest('.player-row-open, button[data-act]');
  if (btn && (btn.dataset.act === 'modal' || btn.classList.contains('player-row-open'))) {
    evt.preventDefault();
    evt.stopPropagation();
    const card = btn.closest('.player[data-steam], .player[data-name]');
    const steamId = btn.dataset.steam || card?.dataset.steam || null;
    const name = btn.dataset.name || card?.dataset.name || null;
    const runtimeKey = btn.dataset.runtime || card?.dataset.runtime || '';
    const profileId = btn.dataset.profile || card?.dataset.profile || '';
    openPlayerActionModal(steamId, name, runtimeKey, btn.dataset.tab || 'items', profileId);
    return;
  }
  if (!btn) {
    const card = evt.target.closest('.player[data-steam], .player[data-name]');
    if (!card) return;
    const player = findPlayerByIdentity(card.dataset.steam || '', card.dataset.name || '', card.dataset.runtime || '') || {
      steamId: card.dataset.steam || '',
      name: card.dataset.name || '',
      runtimeKey: card.dataset.runtime || '',
      profileId: card.dataset.profile || ''
    };
    setSelectedPlayerTarget(player.steamId || '', player.name || '', player.runtimeKey || '', player.profileId || '', player);
    renderPlayers();
    return;
  }
  const steamId = btn.dataset.steam || null;
  const name = btn.dataset.name || null;
  const runtimeKey = btn.dataset.runtime || '';
  const profileId = btn.dataset.profile || '';
  if (btn.dataset.act === 'open-card') {
    const player = findPlayerByIdentity(steamId, name, runtimeKey) || { steamId, name, runtimeKey, profileId };
    openPlayerProfile(player);
    return;
  }
  if (btn.dataset.act === 'details') {
    openPlayerActionModal(steamId, name, runtimeKey, 'inventory', profileId);
    await showActionResult(els.playerActionResult || els.quickResult, () => api(`/api/player-details?steamId=${encodeURIComponent(steamId || '')}&name=${encodeURIComponent(name || '')}&runtimeKey=${encodeURIComponent(runtimeKey || '')}`));
    return;
  }
});
els.playerActionClose.addEventListener('click', closePlayerActionModal);
els.playerActionModal.addEventListener('click', evt => {
  if (evt.target === els.playerActionModal) evt.stopPropagation();
});
if (els.profClose) els.profClose.addEventListener('click', closePlayerProfile);
if (els.playerProfile) els.playerProfile.addEventListener('click', evt => {
  const preset = evt.target.closest('[data-profile-preset]');
  if (!preset || !els.profItemSearch) return;
  els.profItemSearch.value = preset.dataset.profilePreset || '';
  els.profItemSearch.focus();
});
if (els.playersList) els.playersList.addEventListener('keydown', evt => {
  if (evt.key !== 'Enter' && evt.key !== ' ') return;
  const card = evt.target.closest('.player[data-steam], .player[data-name]');
  if (!card) return;
  evt.preventDefault();
  const player = findPlayerByIdentity(card.dataset.steam || '', card.dataset.name || '', card.dataset.runtime || '') || {
    steamId: card.dataset.steam || '',
    name: card.dataset.name || '',
    runtimeKey: card.dataset.runtime || '',
    profileId: card.dataset.profile || ''
  };
  setSelectedPlayerTarget(player.steamId || '', player.name || '', player.runtimeKey || '', player.profileId || '', player);
  renderPlayers();
});
if (els.profSpawnBtn) els.profSpawnBtn.addEventListener('click', async () => {
  if (!state.selectedPlayerTarget) {
    toast('Сначала выбери игрока.');
    return;
  }
  const itemId = (els.profItemSearch && els.profItemSearch.value || '').trim();
  if (!itemId) {
    showResult(els.profActionResult, { ok: false, error: 'Укажи ID предмета.' });
    return;
  }
  const quantity = Math.max(1, Number(els.profItemAmount && els.profItemAmount.value || 1));
  const body = Object.assign(selectedPlayerBody(), { itemId, quantity });
  await showActionResult(els.profActionResult, () => api('/api/player/grant-item', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedInventorySoon();
});
if (els.profTeleportBtn) els.profTeleportBtn.addEventListener('click', async () => {
  if (!state.selectedPlayerTarget) {
    toast('Сначала выбери игрока.');
    return;
  }
  const body = Object.assign(selectedPlayerBody(), {
    x: Number(els.profX && els.profX.value || 0),
    y: Number(els.profY && els.profY.value || 0),
    z: Number(els.profZ && els.profZ.value || 0)
  });
  await showActionResult(els.profActionResult, () => api('/api/player/teleport', { method: 'POST', body: JSON.stringify(body) }));
});
if (els.profOpenPositionBtn) els.profOpenPositionBtn.addEventListener('click', () => openSelectedPlayerModal('position'));
if (els.profStatsBtn) els.profStatsBtn.addEventListener('click', () => openSelectedPlayerModal('stats'));
if (els.profInventoryBtn) els.profInventoryBtn.addEventListener('click', () => openSelectedPlayerModal('inventory'));
if (els.profBalanceBtn) els.profBalanceBtn.addEventListener('click', () => openSelectedPlayerModal('economy'));
if (els.profCharacterBtn) els.profCharacterBtn.addEventListener('click', () => openSelectedPlayerModal('character'));
if (els.profVehicleBtn) els.profVehicleBtn.addEventListener('click', () => openSelectedPlayerModal('vehicle'));
if (els.profKickBtn) els.profKickBtn.addEventListener('click', async () => {
  if (!state.selectedPlayerTarget) {
    toast('Сначала выбери игрока.');
    return;
  }
  if (!confirmDanger('Кикнуть выбранного игрока с сервера?')) return;
  const body = Object.assign(selectedPlayerBody(), { reason: (els.profReason && els.profReason.value || '').trim() || 'действие администратора через панель' });
  await showActionResult(els.profActionResult, () => api('/api/player/kick', { method: 'POST', body: JSON.stringify(body) }));
});
if (els.profBanBtn) els.profBanBtn.addEventListener('click', async () => {
  if (!state.selectedPlayerTarget) {
    toast('Сначала выбери игрока.');
    return;
  }
  if (!confirmDanger('Забанить выбранного игрока?')) return;
  const body = Object.assign(selectedPlayerBody(), { reason: (els.profReason && els.profReason.value || '').trim() || 'действие администратора через панель' });
  await showActionResult(els.profActionResult, () => api('/api/player/ban', { method: 'POST', body: JSON.stringify(body) }));
});
document.querySelectorAll('[data-player-tab]').forEach(btn => {
  btn.addEventListener('click', () => setPlayerActionTab(btn.dataset.playerTab));
});
if (els.playerStatsRefresh) els.playerStatsRefresh.addEventListener('click', () => {
  if (!state.selectedPlayerTarget) return;
  loadPlayerStats(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey, true).catch(toast);
});
if (els.playerCharacterFactsRefresh) els.playerCharacterFactsRefresh.addEventListener('click', () => {
  if (!state.selectedPlayerTarget) return;
  loadPlayerFacts(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey, true).catch(toast);
});
if (els.playerEconomyRefresh) els.playerEconomyRefresh.addEventListener('click', () => {
  if (!state.selectedPlayerTarget) return;
  loadPlayerFacts(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey, true).catch(toast);
});
if (els.playerInventoryRefresh) els.playerInventoryRefresh.addEventListener('click', () => {
  if (!state.selectedPlayerTarget) return;
  loadPlayerInventory(state.selectedPlayerTarget.steamId, state.selectedPlayerTarget.name, state.selectedPlayerTarget.runtimeKey, true).catch(toast);
});
if (els.playerInventoryAddItem) els.playerInventoryAddItem.addEventListener('click', async () => {
  if (!state.selectedPlayerTarget) return;
  const target = modalTargetBody();
  const itemId = (els.playerInventoryAddItemId && els.playerInventoryAddItemId.value || '').trim();
  const quantity = Number(els.playerInventoryAddItemQty && els.playerInventoryAddItemQty.value || 1);
  await showActionResult(els.playerActionResult, async () => {
    const result = await addItemToInventory(itemId, quantity, target);
    refreshSelectedInventorySoon(1800);
    return result;
  });
});
if (els.playerInventoryBody) els.playerInventoryBody.addEventListener('click', async evt => {
  const btn = evt.target.closest('[data-inventory-delete]');
  if (!btn || !state.selectedPlayerTarget) return;
  const entityId = btn.dataset.inventoryDelete || '';
  const itemId = btn.dataset.inventoryItemId || '';
  const label = itemId || entityId || 'предмет';
  if (!entityId) {
    toast('У предмета нет EntityID, удалить его точечно нельзя.');
    return;
  }
  if (!confirmDanger(`Удалить предмет ${label} у выбранного игрока?`)) return;
  const target = modalTargetBody();
  await showActionResult(els.playerActionResult, async () => {
    const queued = await destroyInventoryEntitiesFast([entityId], target);
    refreshSelectedInventorySoon(1600);
    return {
      ok: queued && queued.ok !== false,
      message: `Удаление отправлено: ${label}. Инвентарь обновится автоматически после ответа игры.`,
      queued
    };
  });
});
els.playerActionSendMessage.addEventListener('click', async () => {
  const target = modalTargetBody();
  await showActionResult(els.playerActionResult, () => api('/api/chat', { method: 'POST', body: JSON.stringify({ targetSteamId: target.steamId, targetName: target.name, targetRuntimeKey: target.runtimeKey, runtimeKey: target.runtimeKey, message: scumChatSafeMessage(els.playerActionMessage.value), channel: 'server' }) }));
  refreshChat().catch(() => {});
});
els.playerActionGrantItem.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), { itemId: els.playerActionItemId.value.trim(), quantity: Number(els.playerActionItemQty.value || 1) });
  await showActionResult(els.playerActionResult, () => api('/api/player/grant-item', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedInventorySoon();
});
els.playerActionEquipItem.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), { itemId: els.playerActionItemId.value.trim(), quantity: Number(els.playerActionItemQty.value || 1), equip: true });
  await showActionResult(els.playerActionResult, () => api('/api/player/equip-item', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedInventorySoon();
});
els.playerActionPrewarmItem.addEventListener('click', async () => {
  const body = { itemId: els.playerActionItemId.value.trim(), quantity: Number(els.playerActionItemQty.value || 1) };
  await showActionResult(els.playerActionResult, () => api('/api/items/prewarm', { method: 'POST', body: JSON.stringify(body) }));
});
function welcomeActionResultEl(evt) {
  const id = evt && evt.currentTarget ? String(evt.currentTarget.id || '') : '';
  return id.includes('Profile') ? (els.profActionResult || els.playerActionResult) : els.playerActionResult;
}

async function claimSelectedWelcomePack(evt) {
  await showActionResult(welcomeActionResultEl(evt), async () => {
    const target = modalTargetBody();
    const result = await api('/api/welcome-pack/claim', { method: 'POST', body: JSON.stringify(target) });
    markWelcomePackReceivedLocal(target);
    await refreshWelcomeTimers().catch(() => updatePlayerWelcomeControls());
    return result;
  });
  refreshSelectedInventorySoon(2200);
}

async function resetSelectedWelcomePack(evt) {
  const target = modalTargetBody();
  const label = target.steamId || target.name || 'игрок';
  if (!confirmDanger(`Снять отметку стартпака для ${label}? После этого игрок сможет получить стартовый набор заново.`)) return;
  await showActionResult(welcomeActionResultEl(evt), async () => {
    const result = await api('/api/welcome-pack/reset-timer', { method: 'POST', body: JSON.stringify(target) });
    clearWelcomePackReceivedLocal(target);
    await refreshWelcomeTimers();
    return result;
  });
}

if (els.playerActionWelcome) els.playerActionWelcome.addEventListener('click', claimSelectedWelcomePack);
if (els.playerActionWelcomeProfile) els.playerActionWelcomeProfile.addEventListener('click', claimSelectedWelcomePack);
if (els.playerActionWelcomeCharacter) els.playerActionWelcomeCharacter.addEventListener('click', claimSelectedWelcomePack);
if (els.playerActionWelcomeReset) els.playerActionWelcomeReset.addEventListener('click', resetSelectedWelcomePack);
if (els.playerActionWelcomeResetProfile) els.playerActionWelcomeResetProfile.addEventListener('click', resetSelectedWelcomePack);
if (els.playerActionWelcomeResetCharacter) els.playerActionWelcomeResetCharacter.addEventListener('click', resetSelectedWelcomePack);
els.playerActionClearInventory.addEventListener('click', async () => {
  if (!confirmDanger('Очистить инвентарь выбранного игрока?')) return;
  await showActionResult(els.playerActionResult, async () => {
    const target = modalTargetBody();
    const inventory = await loadPlayerInventory(target.steamId, target.name, target.runtimeKey, true);
    const rows = Array.isArray(inventory && inventory.rows) ? inventory.rows : [];
    const items = rows
      .filter(row => row && row.entityId)
      .sort((a, b) => Number(b.depth || 0) - Number(a.depth || 0));
    if (!items.length) {
      return { ok: false, error: 'В панели нет списка предметов для удаления. Обнови инвентарь или попроси игрока открыть его.' };
    }
    const sentIds = uniqueInventoryEntityIds(items);
    const failed = [];
    let sent = 0;
    let destroyResult = null;
    try {
      destroyResult = await destroyInventoryEntitiesFast(sentIds, target);
      sent = sentIds.length;
    } catch (err) {
      failed.push(err && err.message ? err.message : String(err));
    }
    refreshSelectedInventorySoon(Math.min(3500, 1200 + Number((destroyResult && destroyResult.chunks) || 1) * 350));
    return {
      ok: failed.length === 0 && (!destroyResult || destroyResult.ok !== false),
      message: `Команды очистки отправлены: ${sent}/${items.length}. Инвентарь перечитается автоматически после обновления данных.`,
      failed
    };
  });
});
els.playerActionTeleport.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), { x: Number(els.playerActionX.value || 0), y: Number(els.playerActionY.value || 0), z: Number(els.playerActionZ.value || 0) });
  await showActionResult(els.playerActionResult, () => api('/api/player/teleport', { method: 'POST', body: JSON.stringify(body) }));
});
els.playerActionSetHome.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), { label: els.playerActionHomeLabel.value.trim() || 'home' });
  await showActionResult(els.playerActionResult, () => api('/api/home/set', { method: 'POST', body: JSON.stringify(body) }));
  refreshHomes().catch(() => {});
});
els.playerActionMoney.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), { currency: els.playerActionCurrency.value, amount: Number(els.playerActionAmount.value || 0) });
  if (!Number.isFinite(body.amount) || body.amount === 0) {
    toast('Укажи ненулевую сумму изменения баланса.');
    return;
  }
  await showActionResult(els.playerActionResult, () => api('/api/player/change-money', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedFactsSoon();
});
els.playerActionFame.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), { amount: Number(els.playerActionAmount.value || 0) });
  await showActionResult(els.playerActionResult, () => api('/api/player/change-fame', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedFactsSoon();
});
els.playerActionAttributes.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), {
    strength: Number(els.playerActionStrength.value || 0),
    constitution: Number(els.playerActionConstitution.value || 0),
    dexterity: Number(els.playerActionDexterity.value || 0),
    intelligence: Number(els.playerActionIntelligence.value || 0)
  });
  await showActionResult(els.playerActionResult, () => api('/api/player/set-attributes', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedFactsSoon();
});
if (els.playerActionCharacterPackApply) els.playerActionCharacterPackApply.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), { pack: (els.playerActionCharacterPack && els.playerActionCharacterPack.value.trim()) || 'fullstats' });
  await showActionResult(els.playerActionResult, () => api('/api/player/apply-character-pack', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedFactsSoon(1800);
});
if (els.playerActionFullStats) els.playerActionFullStats.addEventListener('click', async () => {
  if (els.playerActionCharacterPack) els.playerActionCharacterPack.value = 'fullstats';
  const body = Object.assign(modalTargetBody(), { pack: 'fullstats' });
  await showActionResult(els.playerActionResult, () => api('/api/player/apply-character-pack', { method: 'POST', body: JSON.stringify(body) }));
  refreshSelectedFactsSoon(1800);
});
els.playerActionSkillApply.addEventListener('click', async () => {
  if (isAllSkillsValue(els.playerActionSkill.value)) {
    await showActionResult(els.playerActionResult, grantAllSkillsToSelectedPlayer);
    return;
  }
  const body = Object.assign(modalTargetBody(), { skill: els.playerActionSkill.value.trim(), level: Number(els.playerActionSkillLevel.value || 0) });
  await showActionResult(els.playerActionResult, () => api('/api/player/set-skill', { method: 'POST', body: JSON.stringify(body) }));
});
if (els.playerActionAllSkills) els.playerActionAllSkills.addEventListener('click', async () => {
  await showActionResult(els.playerActionResult, grantAllSkillsToSelectedPlayer);
});
els.playerActionSpawnVehicle.addEventListener('click', async () => {
  const vehicleId = els.playerActionVehicleId.value.trim();
  if (!vehicleId) {
    toast('Выбери транспорт из каталога или введи ID транспорта.');
    return;
  }
  const body = Object.assign(modalTargetBody(), {
    vehicleId,
    minutes: Number(els.playerActionRentalMinutes && els.playerActionRentalMinutes.value || 0)
  });
  await showActionResult(els.playerActionResult, async () => {
    const result = await api('/api/player/spawn-vehicle', { method: 'POST', body: JSON.stringify(body) });
    const entityId = result && (result.entityId || (result.data && result.data.entityId));
    if (entityId && els.playerActionVehicleEntityId) els.playerActionVehicleEntityId.value = entityId;
    return result;
  });
});
els.playerActionRentVehicle.addEventListener('click', async () => {
  const vehicleId = els.playerActionVehicleId.value.trim();
  if (!vehicleId) {
    toast('Выбери транспорт для аренды.');
    return;
  }
  const body = Object.assign(modalTargetBody(), {
    vehicleId,
    minutes: Number(els.playerActionRentalMinutes && els.playerActionRentalMinutes.value || 10)
  });
  await showActionResult(els.playerActionResult, async () => {
    const result = await api('/api/vehicle-rental/rent', { method: 'POST', body: JSON.stringify(body) });
    if (result && result.entityId && els.playerActionVehicleEntityId) els.playerActionVehicleEntityId.value = result.entityId;
    refreshSelectedFactsSoon(1800);
    return result;
  });
});
if (els.playerActionDestroyVehicle) els.playerActionDestroyVehicle.addEventListener('click', async () => {
  const body = Object.assign(modalTargetBody(), {
    entityId: (els.playerActionVehicleEntityId && els.playerActionVehicleEntityId.value || '').trim(),
    reason: 'panel-player-card'
  });
  await showActionResult(els.playerActionResult, () => api('/api/vehicle/destroy', { method: 'POST', body: JSON.stringify(body) }));
});
els.playerActionKick.addEventListener('click', async () => {
  if (!confirmDanger('Кикнуть выбранного игрока с сервера?')) return;
  const body = Object.assign(modalTargetBody(), { reason: els.playerActionReason.value.trim() || 'действие администратора через панель' });
  await showActionResult(els.playerActionResult, () => api('/api/player/kick', { method: 'POST', body: JSON.stringify(body) }));
});
els.playerActionBan.addEventListener('click', async () => {
  if (!confirmDanger('Забанить выбранного игрока?')) return;
  const body = Object.assign(modalTargetBody(), { reason: els.playerActionReason.value.trim() || 'действие администратора через панель' });
  await showActionResult(els.playerActionResult, () => api('/api/player/ban', { method: 'POST', body: JSON.stringify(body) }));
});
els.moduleList.addEventListener('click', evt => {
  const card = evt.target.closest('[data-module]');
  if (!card) return;
  if (card.dataset.module !== state.selectedModule) {
    state.wargmRuleEditIndex = null;
    state.wargmRuleSearch = '';
    closeSchedulerJobEditor();
    state.schedulerJobSearch = '';
    resetSimpleModuleItemEditor();
    state.simpleSettingEditPath = null;
    state.simpleModuleSearch = '';
    if (isExternalShopModule(card.dataset.module)) state.wargmConfigTab = 'settings';
    if (String(card.dataset.module || '').toLowerCase() === 'scheduled-events') state.schedulerConfigTab = 'settings';
    if (simpleModuleProfile(card.dataset.module)) state.simpleModuleTabs[String(card.dataset.module || '').toLowerCase()] = 'settings';
  }
  state.selectedModule = card.dataset.module;
  renderModules();
});
els.moduleFields.addEventListener('click', async evt => {
  if (handleBattlepassItemsClick(evt)) return;
  const consumeModulePortalClick = () => {
    evt.preventDefault();
    evt.stopPropagation();
  };
  const wargmTab = evt.target.closest('[data-wargm-config-tab]');
  const schedulerTab = evt.target.closest('[data-scheduler-config-tab]');
  const simpleTab = evt.target.closest('[data-simple-config-tab]');
  const openRule = evt.target.closest('[data-wargm-rule-open]');
  const closeRule = evt.target.closest('[data-wargm-rule-close]');
  const saveRule = evt.target.closest('[data-wargm-rule-save]');
  const overlay = evt.target && evt.target.dataset && evt.target.dataset.wargmRuleOverlay === 'true';
  const openSimpleItem = evt.target.closest('[data-simple-item-open]');
  const closeSimpleItem = evt.target.closest('[data-simple-item-close]');
  const saveSimpleItem = evt.target.closest('[data-simple-item-save]');
  const simpleOverlay = evt.target && evt.target.dataset && evt.target.dataset.simpleItemOverlay === 'true';
  const openSimpleSetting = evt.target.closest('[data-simple-setting-open]');
  const closeSimpleSetting = evt.target.closest('[data-simple-setting-close]');
  const saveSimpleSetting = evt.target.closest('[data-simple-setting-save]');
  const simpleSettingOverlay = evt.target && evt.target.dataset && evt.target.dataset.simpleSettingOverlay === 'true';
  const openSchedulerJob = evt.target.closest('[data-scheduler-job-open]');
  const closeSchedulerJob = evt.target.closest('[data-scheduler-job-close]');
  const saveSchedulerJob = evt.target.closest('[data-scheduler-job-save]');
  const runSchedulerJob = evt.target.closest('[data-scheduler-job-run]');
  const schedulerOverlay = evt.target && evt.target.dataset && evt.target.dataset.schedulerJobOverlay === 'true';
  const add = evt.target.closest('[data-array-add]');
  const remove = evt.target.closest('[data-array-remove]');
  const addRuleItem = evt.target.closest('[data-wargm-rule-item-add]');
  const copySingleRuleItem = evt.target.closest('[data-wargm-rule-item-copy-single]');
  const removeRuleItem = evt.target.closest('[data-wargm-rule-item-remove]');
  const saveScheduler = evt.target.closest('[data-scheduler-save]');
  const worldEditAction = evt.target.closest('[data-world-edit-action]');
  if (worldEditAction) {
    await runWorldEditAction(worldEditAction.dataset.worldEditAction || 'spawn');
    return;
  }
  if (wargmTab) {
    state.wargmConfigTab = wargmTab.dataset.wargmConfigTab === 'products' ? 'products' : 'settings';
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (schedulerTab) {
    const next = schedulerTab.dataset.schedulerConfigTab || 'settings';
    state.schedulerConfigTab = ['settings', 'jobs', 'resources'].includes(next) ? next : 'settings';
    if (state.schedulerConfigTab !== 'jobs') closeSchedulerJobEditor();
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (simpleTab) {
    const key = String(state.selectedModule || '').toLowerCase();
    const requested = simpleTab.dataset.simpleConfigTab || 'settings';
    const allowed = simpleModuleAllowedTabs(simpleModuleProfile());
    state.simpleModuleTabs[key] = allowed.includes(requested) ? requested : 'settings';
    resetSimpleModuleItemEditor();
    state.simpleSettingEditPath = null;
    state.simpleModuleSearch = '';
    const selected = state.modules.find(m => m.key === state.selectedModule);
    if (selected) renderModuleActions(selected);
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (closeSimpleSetting) {
    consumeModulePortalClick();
    state.simpleSettingEditPath = null;
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (saveSimpleSetting) {
    consumeModulePortalClick();
    try {
      const modal = saveSimpleSetting.closest('[data-simple-setting-portal="true"], [data-simple-setting-overlay="true"]');
      const input = modal && modal.querySelector('[data-simple-setting-path]');
      if (input) syncSimpleSettingInput(input, true);
      await showActionResult(els.quickResult, saveSelectedModule);
      state.simpleSettingEditPath = null;
      renderModuleFields(state.moduleDraft);
    } catch (err) {
      toast(err);
    }
    return;
  }
  if (openSimpleSetting) {
    state.simpleSettingEditPath = openSimpleSetting.dataset.simpleSettingOpen || '';
    resetSimpleModuleItemEditor();
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (closeSimpleItem) {
    consumeModulePortalClick();
    resetSimpleModuleItemEditor();
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (saveSimpleItem) {
    consumeModulePortalClick();
    try {
      await showActionResult(els.quickResult, () => saveSimpleModuleItemEditor({ persist: true }));
    } catch (err) {
      toast(err);
    }
    return;
  }
  if (openSimpleItem && !evt.target.closest('[data-array-remove]')) {
    const arrayKey = openSimpleItem.dataset.simpleItemArrayKey || (openSimpleItem.closest('[data-simple-array-key]') && openSimpleItem.closest('[data-simple-array-key]').dataset.simpleArrayKey) || simpleModuleTabArrayKey(state.moduleDraft || {}, simpleModuleProfile());
    beginSimpleModuleEdit(arrayKey, Number(openSimpleItem.dataset.simpleItemOpen));
    return;
  }
  if (saveScheduler) {
    showActionResult(els.quickResult, saveSelectedModule).catch(toast);
    return;
  }
  if (closeSchedulerJob) {
    consumeModulePortalClick();
    closeSchedulerJobEditor();
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (saveSchedulerJob) {
    consumeModulePortalClick();
    try {
      await showActionResult(els.quickResult, saveSelectedModule);
      finishSchedulerJobEditor();
      renderModuleFields(state.moduleDraft);
    } catch (err) {
      toast(err);
    }
    return;
  }
  if (runSchedulerJob) {
    consumeModulePortalClick();
    const index = Number(runSchedulerJob.dataset.schedulerJobRun);
    const jobsKey = schedulerArrayKey(state.moduleDraft || {}, 'Jobs', 'Jobs');
    const job = state.moduleDraft && Array.isArray(state.moduleDraft[jobsKey]) ? state.moduleDraft[jobsKey][index] : null;
    if (!job) return;
    const enabled = schedulerValue(job, 'Enabled', true) !== false;
    if (!enabled && !confirmDanger('Задание выключено. Всё равно запустить его один раз для проверки?')) return;
    try {
      await showActionResult(els.quickResult, async () => {
        await saveSelectedModule();
        return api(`/api/scheduled-events/run?index=${encodeURIComponent(index)}`, { method: 'POST', body: JSON.stringify({ index, force: true }) });
      });
    } catch (err) {
      toast(err);
    }
    return;
  }
  if (openSchedulerJob && !evt.target.closest('[data-array-remove]')) {
    openSchedulerJobEditor(openSchedulerJob.dataset.schedulerJobOpen, false);
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (closeRule) {
    state.wargmRuleEditIndex = null;
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (saveRule) {
    try {
      await showActionResult(els.quickResult, saveSelectedModule);
      state.wargmRuleEditIndex = null;
      renderModuleFields(state.moduleDraft);
    } catch (err) {
      toast(err);
    }
    return;
  }
  if (openRule) {
    state.wargmRuleEditIndex = Number(openRule.dataset.wargmRuleOpen);
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (addRuleItem) {
    const index = Number(addRuleItem.dataset.wargmRuleItemAdd);
    const key = 'Rules';
    const actualKey = state.moduleDraft && Array.isArray(state.moduleDraft.Rules) ? 'Rules' : 'rules';
    if (!state.moduleDraft || !Array.isArray(state.moduleDraft[actualKey]) || !state.moduleDraft[actualKey][index]) return;
    const rule = state.moduleDraft[actualKey][index];
    const itemKey = Array.isArray(rule.items) && !Array.isArray(rule.Items) ? 'items' : 'Items';
    if (!Array.isArray(rule[itemKey])) rule[itemKey] = [];
    rule[itemKey].push(itemKey === 'items' ? { itemId: 'Apple_2', quantity: 1 } : { ItemId: 'Apple_2', Quantity: 1 });
    setModuleDraftText();
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (copySingleRuleItem) {
    const index = Number(copySingleRuleItem.dataset.wargmRuleItemCopySingle);
    const actualKey = state.moduleDraft && Array.isArray(state.moduleDraft.Rules) ? 'Rules' : 'rules';
    if (!state.moduleDraft || !Array.isArray(state.moduleDraft[actualKey]) || !state.moduleDraft[actualKey][index]) return;
    const rule = state.moduleDraft[actualKey][index];
    const itemKey = Array.isArray(rule.items) && !Array.isArray(rule.Items) ? 'items' : 'Items';
    const id = wargmRuleValue(rule, 'ItemId', 'Apple_2') || 'Apple_2';
    const quantity = Math.max(1, Number(wargmRuleValue(rule, 'Quantity', 1) || 1));
    if (!Array.isArray(rule[itemKey])) rule[itemKey] = [];
    rule[itemKey].push(itemKey === 'items' ? { itemId: id, quantity } : { ItemId: id, Quantity: quantity });
    setModuleDraftText();
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (removeRuleItem) {
    const index = Number(removeRuleItem.dataset.wargmRuleItemRemove);
    const itemIndex = Number(removeRuleItem.dataset.ruleItemIndex);
    const actualKey = state.moduleDraft && Array.isArray(state.moduleDraft.Rules) ? 'Rules' : 'rules';
    if (state.moduleDraft && Array.isArray(state.moduleDraft[actualKey]) && state.moduleDraft[actualKey][index]) {
      const rule = state.moduleDraft[actualKey][index];
      const itemKey = Array.isArray(rule.items) && !Array.isArray(rule.Items) ? 'items' : 'Items';
      if (!Array.isArray(rule[itemKey])) return;
      rule[itemKey].splice(itemIndex, 1);
      setModuleDraftText();
      renderModuleFields(state.moduleDraft);
    }
    return;
  }
  if (add) {
    const key = add.dataset.arrayAdd;
    const simpleProfileForAdd = simpleModuleProfile();
    if (simpleProfileForAdd && simpleModuleArrayMatch(simpleProfileForAdd, key)) {
      beginSimpleModuleCreate(key);
      return;
    }
    if (!Array.isArray(state.moduleDraft[key])) state.moduleDraft[key] = [];
    state.moduleDraft[key].push(defaultArrayItem(key, state.selectedModule));
    if (isExternalShopModule() && String(key).toLowerCase() === 'rules') state.wargmRuleEditIndex = state.moduleDraft[key].length - 1;
    if (String(key).toLowerCase() === 'jobs') {
      openSchedulerJobEditor(state.moduleDraft[key].length - 1, true);
      state.schedulerConfigTab = 'jobs';
    }
    const simpleProfile = simpleModuleProfile();
    if (simpleProfile && String(key).toLowerCase() === simpleModuleArrayKey(state.moduleDraft || {}, simpleProfile).toLowerCase()) {
      state.simpleModuleEditIndex = state.moduleDraft[key].length - 1;
      state.simpleModuleEditArrayKey = key;
      state.simpleModuleTabs[simpleProfile.key] = 'items';
    }
    if (simpleProfile && String(key).toLowerCase() === simpleModuleVipArrayKey(state.moduleDraft || {}, simpleProfile).toLowerCase()) {
      state.simpleModuleEditIndex = state.moduleDraft[key].length - 1;
      state.simpleModuleEditArrayKey = key;
      state.simpleModuleTabs[simpleProfile.key] = 'vip-items';
    }
    if (simpleProfile) {
      const extra = simpleModuleExtraArrays(simpleProfile).find(entry => String(key).toLowerCase() === simpleModuleExtraArrayKey(state.moduleDraft || {}, entry).toLowerCase());
      if (extra) {
        state.simpleModuleEditIndex = state.moduleDraft[key].length - 1;
        state.simpleModuleEditArrayKey = key;
        state.simpleModuleTabs[simpleProfile.key] = extra.tab;
      }
    }
    setModuleDraftText();
    renderModuleFields(state.moduleDraft);
    return;
  }
  if (remove) {
    const key = remove.dataset.arrayRemove;
    const index = Number(remove.dataset.arrayIndex);
    if (Array.isArray(state.moduleDraft[key])) state.moduleDraft[key].splice(index, 1);
    if (String(key).toLowerCase() === 'rules') {
      if (state.wargmRuleEditIndex === index) state.wargmRuleEditIndex = null;
      else if (state.wargmRuleEditIndex > index) state.wargmRuleEditIndex -= 1;
    }
    if (String(key).toLowerCase() === 'jobs') {
      if (state.schedulerJobEditIndex === index) finishSchedulerJobEditor();
      else if (state.schedulerJobEditIndex > index) state.schedulerJobEditIndex -= 1;
    }
    const simpleProfile = simpleModuleProfile();
    if (simpleProfile && String(key).toLowerCase() === (state.simpleModuleEditArrayKey || '').toLowerCase()) {
      if (state.simpleModuleEditIndex === index) resetSimpleModuleItemEditor();
      else if (state.simpleModuleEditIndex > index) state.simpleModuleEditIndex -= 1;
      if (state.simpleModuleEditIndex == null) state.simpleModuleEditArrayKey = null;
    }
    setModuleDraftText();
    renderModuleFields(state.moduleDraft);
    return;
  }
});
els.moduleFields.addEventListener('input', evt => {
  if (evt.target && evt.target.matches && evt.target.matches('[data-world-edit-field]')) {
    syncWorldEditField(evt.target);
    return;
  }
  if (evt.target && evt.target.matches && evt.target.matches('[data-wargm-rule-search]')) {
    updateWargmRuleSearchFilter();
    return;
  }
  if (evt.target && evt.target.matches && evt.target.matches('[data-scheduler-job-search]')) {
    updateSchedulerJobSearchFilter();
    return;
  }
  if (evt.target && evt.target.matches && evt.target.matches('[data-simple-item-search]')) {
    updateSimpleModuleSearchFilter();
    return;
  }
  try { syncModuleField(evt.target); } catch (err) { toast(err); }
});
els.moduleFields.addEventListener('focusin', evt => {
  if (evt.target.matches('input[list="itemCatalog"], input[list="vehicleCatalog"], input[list="skillCatalog"]')) {
    state.lastCatalogInput = evt.target;
  }
});
els.moduleFields.addEventListener('change', evt => {
  try {
    if (evt.target && evt.target.matches && evt.target.matches('[data-world-edit-field]')) {
      const field = evt.target.dataset.worldEditField || '';
      syncWorldEditField(evt.target);
      if (field === 'PresetIndex') renderModuleFields(state.moduleDraft);
      return;
    }
    syncModuleField(evt.target);
    if (evt.target && evt.target.dataset && evt.target.dataset.simpleSettingInline === 'true') {
      renderModuleFields(state.moduleDraft);
      return;
    }
    if (String(state.selectedModule || '').toLowerCase() === 'scheduled-events' &&
        evt.target && evt.target.dataset && evt.target.dataset.arrayField === 'Mode') {
      renderModuleFields(state.moduleDraft);
      return;
    }
    if (evt.target && evt.target.dataset && evt.target.dataset.arrayField === 'DeliveryMode') {
      if (evt.target.value === 'AllSkills') {
        const key = evt.target.dataset.arrayKey;
        const index = Number(evt.target.dataset.arrayIndex);
        const row = state.moduleDraft && Array.isArray(state.moduleDraft[key]) ? state.moduleDraft[key][index] : null;
        if (row && !Number(row.SkillLevel || row.skillLevel || 0)) row.SkillLevel = 4;
      }
      if ((evt.target.value === 'Gold' || evt.target.value === 'Fame') && state.moduleDraft) {
        const key = evt.target.dataset.arrayKey;
        const index = Number(evt.target.dataset.arrayIndex);
        const row = Array.isArray(state.moduleDraft[key]) ? state.moduleDraft[key][index] : null;
        if (row && !Number(row.Amount || row.amount || 0)) row.Amount = 1000;
      }
      if (evt.target.value === 'Vip' && state.moduleDraft) {
        const key = evt.target.dataset.arrayKey;
        const index = Number(evt.target.dataset.arrayIndex);
        const row = Array.isArray(state.moduleDraft[key]) ? state.moduleDraft[key][index] : null;
        if (row && !Number(row.DurationDays || row.durationDays || 0)) row.DurationDays = 30;
        if (row && !String(row.Tier || row.tier || '').trim()) row.Tier = 'vip';
      }
      renderModuleFields(state.moduleDraft);
      return;
    }
    if (isExternalShopModule() &&
        evt.target && evt.target.dataset && evt.target.dataset.arrayKey &&
        ['ItemId', 'VehicleAsset'].includes(evt.target.dataset.arrayField || '')) {
      renderModuleFields(state.moduleDraft);
      return;
    }
  } catch (err) { toast(err); }
});
document.addEventListener('click', async evt => {
  if (evt.defaultPrevented) return;
  const modal = evt.target.closest('[data-scheduler-job-portal="true"]');
  if (!modal) return;
  const closeSchedulerJob = evt.target.closest('[data-scheduler-job-close]');
  const saveSchedulerJob = evt.target.closest('[data-scheduler-job-save]');
  const runSchedulerJob = evt.target.closest('[data-scheduler-job-run]');
  const schedulerOverlay = evt.target && evt.target.dataset && evt.target.dataset.schedulerJobOverlay === 'true';

  if (closeSchedulerJob) {
    evt.preventDefault();
    evt.stopPropagation();
    closeSchedulerJobEditor();
    renderModuleFields(state.moduleDraft);
    return;
  }

  if (saveSchedulerJob) {
    evt.preventDefault();
    evt.stopPropagation();
    try {
      await showActionResult(els.quickResult, saveSelectedModule);
      finishSchedulerJobEditor();
      renderModuleFields(state.moduleDraft);
    } catch (err) {
      toast(err);
    }
    return;
  }

  if (runSchedulerJob) {
    evt.preventDefault();
    evt.stopPropagation();
    const index = Number(runSchedulerJob.dataset.schedulerJobRun);
    const jobsKey = schedulerArrayKey(state.moduleDraft || {}, 'Jobs', 'Jobs');
    const job = state.moduleDraft && Array.isArray(state.moduleDraft[jobsKey]) ? state.moduleDraft[jobsKey][index] : null;
    if (!job) return;
    const enabled = schedulerValue(job, 'Enabled', true) !== false;
    if (!enabled && !confirmDanger('Задание выключено. Всё равно запустить его один раз для проверки?')) return;
    try {
      await showActionResult(els.quickResult, async () => {
        await saveSelectedModule();
        return api(`/api/scheduled-events/run?index=${encodeURIComponent(index)}`, { method: 'POST', body: JSON.stringify({ index, force: true }) });
      });
    } catch (err) {
      toast(err);
    }
    return;
  }
});
document.addEventListener('click', async evt => {
  if (evt.defaultPrevented) return;
  const modal = evt.target.closest('[data-simple-item-portal="true"]');
  if (!modal) return;
  if (handleBattlepassItemsClick(evt)) return;
  const closeSimpleItem = evt.target.closest('[data-simple-item-close]');
  const saveSimpleItem = evt.target.closest('[data-simple-item-save]');
  const simpleOverlay = evt.target && evt.target.dataset && evt.target.dataset.simpleItemOverlay === 'true';

  if (closeSimpleItem) {
    evt.preventDefault();
    evt.stopPropagation();
    resetSimpleModuleItemEditor();
    renderModuleFields(state.moduleDraft);
    return;
  }

  if (saveSimpleItem) {
    evt.preventDefault();
    evt.stopPropagation();
    try {
      await showActionResult(els.quickResult, () => saveSimpleModuleItemEditor({ persist: true }));
    } catch (err) {
      toast(err);
    }
  }
});
document.addEventListener('click', async evt => {
  if (evt.defaultPrevented) return;
  const modal = evt.target.closest('[data-simple-setting-portal="true"]');
  if (!modal) return;
  if (handleChatLinesEditorClick(evt, modal)) return;
  const closeSimpleSetting = evt.target.closest('[data-simple-setting-close]');
  const saveSimpleSetting = evt.target.closest('[data-simple-setting-save]');
  const simpleSettingOverlay = evt.target && evt.target.dataset && evt.target.dataset.simpleSettingOverlay === 'true';

  if (closeSimpleSetting) {
    evt.preventDefault();
    evt.stopPropagation();
    state.simpleSettingEditPath = null;
    renderModuleFields(state.moduleDraft);
    return;
  }

  if (saveSimpleSetting) {
    evt.preventDefault();
    evt.stopPropagation();
    try {
      const chatEditor = modal.querySelector('[data-chat-lines-editor="true"]');
      if (chatEditor) {
        const issues = chatLineEditorIssues(
          chatLinesForPath(chatEditor.dataset.chatLinesPath || ''),
          chatLineEditorLimits(state.moduleDraft || {})
        );
        if (issues.length) throw new Error(`Исправьте текст /help перед сохранением: ${issues.join(' ')}`);
      } else {
        const input = modal.querySelector('[data-simple-setting-path]');
        if (input) syncSimpleSettingInput(input, true);
      }
      await showActionResult(els.quickResult, saveSelectedModule);
      state.simpleSettingEditPath = null;
      renderModuleFields(state.moduleDraft);
    } catch (err) {
      toast(err);
    }
  }
});
document.addEventListener('input', evt => {
  if (!evt.target.closest || (!evt.target.closest('[data-scheduler-job-portal="true"]') && !evt.target.closest('[data-simple-item-portal="true"]') && !evt.target.closest('[data-simple-setting-portal="true"]'))) return;
  if (evt.target.matches && evt.target.matches('[data-chat-lines-input]')) {
    try { syncChatLinesEditorInput(evt.target); } catch (err) { toast(err); }
    return;
  }
  try { syncModuleField(evt.target); } catch (err) { toast(err); }
});
document.addEventListener('paste', evt => {
  const target = evt.target;
  if (!target || !target.matches || !target.matches('[data-chat-lines-input]')) return;
  const text = evt.clipboardData && evt.clipboardData.getData ? evt.clipboardData.getData('text') : '';
  if (!/[\r\n]/.test(text)) return;
  const editor = target.closest('[data-chat-lines-editor="true"]');
  if (!editor) return;
  evt.preventDefault();
  const result = appendChatLinesForEditor(editor, text, Number(target.dataset.chatLinesIndex));
  if (!result.accepted) toast('В буфере нет непустых строк для добавления.');
  else if (result.rejected) toast(`Добавлено ${formatRussianLineCount(result.accepted)}; остальные строки не поместились в лимит.`);
  renderSimpleSettingPortal(state.moduleDraft);
});
document.addEventListener('change', evt => {
  const schedulerPortal = evt.target.closest && evt.target.closest('[data-scheduler-job-portal="true"]');
  const simplePortal = evt.target.closest && evt.target.closest('[data-simple-item-portal="true"]');
  const simpleSettingPortal = evt.target.closest && evt.target.closest('[data-simple-setting-portal="true"]');
  if (!schedulerPortal && !simplePortal && !simpleSettingPortal) return;
  try {
    syncModuleField(evt.target);
    if (schedulerPortal && evt.target && evt.target.dataset && evt.target.dataset.arrayField === 'Mode') {
      renderModuleFields(state.moduleDraft);
    } else if (schedulerPortal) {
      renderSchedulerJobPortal();
    }
  } catch (err) { toast(err); }
});
document.addEventListener('focusin', evt => {
  if (evt.target.matches('input[list="itemCatalog"], input[list="vehicleCatalog"], input[list="skillCatalog"]')) {
    state.lastCatalogInput = evt.target;
  }
});
if (els.moduleCatalogSearch) els.moduleCatalogSearch.addEventListener('input', renderModuleCatalog);
if (els.moduleCatalogMode) els.moduleCatalogMode.addEventListener('change', renderModuleCatalog);
if (els.moduleCatalogCategories) els.moduleCatalogCategories.addEventListener('click', evt => {
  const btn = evt.target.closest('[data-catalog-category]');
  if (!btn) return;
  state.catalogCategory = btn.dataset.catalogCategory || '';
  renderModuleCatalog();
});
if (els.moduleCatalogGrid) els.moduleCatalogGrid.addEventListener('click', evt => {
  const tile = evt.target.closest('[data-catalog-value]');
  if (!tile) return;
  const value = tile.dataset.catalogValue || '';
  state.catalogSelected = {
    kind: tile.dataset.catalogKind || '',
    value,
    name: tile.dataset.catalogName || value,
    category: tile.dataset.catalogCategory || ''
  };
  const input = state.lastCatalogInput && document.contains(state.lastCatalogInput) ? state.lastCatalogInput : null;
  if (input) {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
    input.focus();
  }
  renderModuleCatalog();
});
if (els.moduleActions) els.moduleActions.addEventListener('click', async evt => {
  const save = evt.target.closest('[data-module-save]');
  const add = evt.target.closest('[data-module-add-array]');
  const stateBtn = evt.target.closest('[data-module-state]');
  const shopQueue = evt.target.closest('[data-shop-queue]');
  const shopAction = evt.target.closest('[data-shop-action]');
  if (save) {
    await showActionResult(els.quickResult, saveSelectedModule);
  }
  if (add) {
    const key = add.dataset.moduleAddArray;
    const simpleProfileForAdd = simpleModuleProfile();
    if (simpleProfileForAdd && simpleModuleArrayMatch(simpleProfileForAdd, key)) {
      beginSimpleModuleCreate(key);
      return;
    }
    if (!Array.isArray(state.moduleDraft[key])) state.moduleDraft[key] = [];
    state.moduleDraft[key].push(defaultArrayItem(key, state.selectedModule));
    if (isExternalShopModule() && String(key).toLowerCase() === 'rules') state.wargmRuleEditIndex = state.moduleDraft[key].length - 1;
    if (String(key).toLowerCase() === 'jobs') {
      openSchedulerJobEditor(state.moduleDraft[key].length - 1, true);
      state.schedulerConfigTab = 'jobs';
    }
    const simpleProfile = simpleModuleProfile();
    if (simpleProfile && String(key).toLowerCase() === simpleModuleArrayKey(state.moduleDraft || {}, simpleProfile).toLowerCase()) {
      state.simpleModuleEditIndex = state.moduleDraft[key].length - 1;
      state.simpleModuleEditArrayKey = key;
      state.simpleModuleTabs[simpleProfile.key] = 'items';
    }
    if (simpleProfile && String(key).toLowerCase() === simpleModuleVipArrayKey(state.moduleDraft || {}, simpleProfile).toLowerCase()) {
      state.simpleModuleEditIndex = state.moduleDraft[key].length - 1;
      state.simpleModuleEditArrayKey = key;
      state.simpleModuleTabs[simpleProfile.key] = 'vip-items';
    }
    setModuleDraftText();
    renderModuleFields(state.moduleDraft);
  }
  if (stateBtn) {
    const key = stateBtn.dataset.moduleState || state.selectedModule || '';
    await showActionResult(els.quickResult, () => api(`/api/module-state?key=${encodeURIComponent(key)}`));
  }
  if (shopQueue) {
    const shop = String(shopQueue.dataset.shopQueue || '');
    if (!['wargm', 'gamestores'].includes(shop)) return;
    await showActionResult(els.quickResult, () => api(`/api/${shop}/pending`));
  }
  if (shopAction) {
    const [shop, actionName] = String(shopAction.dataset.shopAction || '').split(':');
    if (!['wargm', 'gamestores'].includes(shop) || !['sync', 'deliver-pending', 'confirm-delivered', 'process'].includes(actionName)) return;
    await showActionResult(els.quickResult, () => api(`/api/${shop}/${actionName}`, { method: 'POST', body: JSON.stringify({}) }));
  }
});
els.moduleConfig.addEventListener('change', () => {
  try {
    state.moduleDraft = JSON.parse(els.moduleConfig.value);
    renderModuleFields(state.moduleDraft);
  } catch {}
});
if (els.saveModule) els.saveModule.addEventListener('click', async () => {
  await showActionResult(els.quickResult, saveSelectedModule);
});
if (els.reloadModules) els.reloadModules.addEventListener('click', async () => {
  await showActionResult(els.quickResult, async () => {
    const data = await api('/api/status');
    await refreshModules();
    return {
      ok: true,
      message: 'Безопасное обновление выполнено: панель перечитала состояние и конфиги. Серверный процесс не перезапускался.',
      status: data.data || data
    };
  });
});
if (els.deleteModule) els.deleteModule.addEventListener('click', async () => {
  const key = state.selectedModule || '';
  await showActionResult(els.quickResult, async () => {
    const data = await api('/api/plugin-state', { method: 'POST', body: JSON.stringify({ key, enabled: false }) });
    await refreshModules();
    return data;
  });
});

refreshCatalogs().then(() => {
  renderWargmItems();
  updateWargmModeUi();
}).catch(toast);
renderWargmItems();
setServiceTab(state.serviceTab);
setEconomyTab(state.economyTab);
setPage('servers');

const PASSIVE_STATUS_REFRESH_MS = 300000;
window.setInterval(() => {
  if (document.hidden) return;
  if (state.page === 'servers') refreshStatus().catch(() => {});
  if (state.page === 'chat') refreshChat().catch(() => {});
}, PASSIVE_STATUS_REFRESH_MS);
