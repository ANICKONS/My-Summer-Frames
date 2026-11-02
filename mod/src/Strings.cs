// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// The texts of the settings page and the console. English is the reference: every key exists there. Settings
// texts: labels in sentence case, one or two short lines under a section, no internals. Colors are added by the
// page (Palette), not stored here.

using System.Collections.Generic;

namespace MySummerFrames
{
    static class Strings
    {
        public static readonly Dictionary<string, string> En = new Dictionary<string, string>
        {
            // === Information ===
            { "header.info", "Information" },
            { "info.by", "by {0}" },
            { "info.checking", "Checking the video card..." },
            { "info.yes", "yes" },
            { "info.no", "no" },
            { "info.up_to", "up to {0}x" },
            { "info.available", "Available" },
            { "info.unavailable", "Unavailable" },
            { "info.caps_line", "{0}: {1}" },

            // === Language ===
            { "header.language", "Language / Язык" },

            // === Upscaling ===
            { "header.upscaling", "Anti-aliasing and upscaling" },
            { "set.upscaler", "Upscaler" },
            { "upscaler.off", "Off (the game's FXAA)" },
            { "set.mode", "Mode" },
            { "set.sharpness", "Sharpness" },
            { "hint.upscaling", "DLAA / Native AA: the cleanest edges at full resolution.\n" +
                                "Quality ... Ultra Performance: fewer pixels, more FPS on weaker video cards." },
            { "note.dlss_fsr", "DLSS needs a GeForce RTX: FSR runs instead." },
            { "note.no_upscaler", "No upscaler works on this video card." },

            // === Frame generation ===
            { "header.fg", "Frame generation" },
            { "set.fg", "Frame generation" },
            { "set.multiplier", "Multiplier" },
            { "fg.fsr_multiplier", "Multiplier: 2x" },
            { "set.fg_counter", "FG counter under FPS" },
            { "hint.fg", "Adds generated frames: smoother motion, a bit more input lag.\n" +
                         "Needs a window or a borderless window (-popupwindow)." },
            { "note.fg_dlss_fsr", "DLSS frame generation needs an RTX 40 / 50: FSR runs instead." },
            { "note.fg_none", "Frame generation does not work on this video card." },
            { "note.fg_max", "This video card generates up to {0}x." },
            { "note.fg_fullscreen", "Waiting for a window: switch to windowed mode or start the game with -popupwindow." },

            // === Latency ===
            { "header.latency", "Latency" },
            { "set.reflex", "NVIDIA Reflex" },
            { "reflex.boost", "On + Boost" },
            { "hint.reflex", "Lower input lag. Boost keeps the video card's clocks up." },
            { "note.reflex_none", "Reflex works on NVIDIA video cards only." },

            // === Diagnostics ===
            { "header.diagnostics", "Diagnostics" },
            { "set.verbose", "Detailed log in the console" },
            { "hint.verbose", "Technical messages, for bug reports." },

            // === About ===
            { "header.about", "About" },
            { "about.sdks", "NVIDIA DLSS SDK 310.9.16, NVIDIA NVAPI, AMD FSR SDK 2.4.0 (MIT)." },
            { "about.trademarks", "NVIDIA, DLSS, Reflex, AMD and FSR are trademarks of their owners. Not affiliated with NVIDIA or AMD." },

            // === Words ===
            { "state.off", "Off" },
            { "state.on", "On" },
            { "mode.off", "Off" },
            { "mode.dlaa", "DLAA" },
            { "mode.native", "Native AA" },
            { "mode.quality", "Quality" },
            { "mode.balanced", "Balanced" },
            { "mode.performance", "Performance" },
            { "mode.ultra", "Ultra Performance" },

            // === Console ===
            { "con.ready", "{0} ready on {1}" },
            { "con.caps", "DLSS {0} · DLSS FG {1} · FSR {2} · FSR FG {3} · Reflex {4}" },
            { "con.upscaler", "Upscaler: {0}" },
            { "con.fg", "Frame generation: {0}" },
            { "con.reflex", "Reflex: {0}" },
            { "con.sharpness", "Sharpness: {0}" },
            { "con.verbose", "Detailed log: {0}" },
            { "con.plugin", "The native plugin did not start: {0}" },
            { "con.no_device", "No Direct3D 11 device: the mod stays off." },
        };

        public static readonly Dictionary<string, string> Ru = new Dictionary<string, string>
        {
            // === Information ===
            { "header.info", "Информация" },
            { "info.by", "автор: {0}" },
            { "info.checking", "Проверка видеокарты..." },
            { "info.yes", "да" },
            { "info.no", "нет" },
            { "info.up_to", "до {0}x" },
            { "info.available", "Доступно" },
            { "info.unavailable", "Недоступно" },
            { "info.caps_line", "{0}: {1}" },

            // === Upscaling ===
            { "header.upscaling", "Сглаживание и апскейл" },
            { "set.upscaler", "Апскейлер" },
            { "upscaler.off", "Выкл (FXAA игры)" },
            { "set.mode", "Режим" },
            { "set.sharpness", "Резкость" },
            { "hint.upscaling", "DLAA / Native AA — самые чистые края в полном разрешении.\n" +
                                "Качество ... Ультра — меньше пикселей, больше FPS на слабых видеокартах." },
            { "note.dlss_fsr", "DLSS нужна GeForce RTX: вместо неё работает FSR." },
            { "note.no_upscaler", "На этой видеокарте апскейл не работает." },

            // === Frame generation ===
            { "header.fg", "Генерация кадров" },
            { "set.fg", "Генерация кадров" },
            { "set.multiplier", "Множитель" },
            { "fg.fsr_multiplier", "Множитель: 2x" },
            { "set.fg_counter", "Счётчик FG под FPS" },
            { "hint.fg", "Добавляет сгенерированные кадры: плавнее, но чуть больше задержка.\n" +
                         "Нужен оконный режим или окно без рамки (-popupwindow)." },
            { "note.fg_dlss_fsr", "Генерации DLSS нужна RTX 40 / 50: вместо неё работает FSR." },
            { "note.fg_none", "На этой видеокарте генерация кадров не работает." },
            { "note.fg_max", "Эта видеокарта генерирует до {0}x." },
            { "note.fg_fullscreen", "Ждёт окна: включите оконный режим или запустите игру с -popupwindow." },

            // === Latency ===
            { "header.latency", "Задержка" },
            { "set.reflex", "NVIDIA Reflex" },
            { "reflex.boost", "Вкл + Boost" },
            { "hint.reflex", "Меньше задержка ввода. Boost не даёт видеокарте снижать частоты." },
            { "note.reflex_none", "Reflex работает только на видеокартах NVIDIA." },

            // === Diagnostics ===
            { "header.diagnostics", "Диагностика" },
            { "set.verbose", "Подробный лог в консоли" },
            { "hint.verbose", "Технические сообщения — для отчётов об ошибках." },

            // === About ===
            { "header.about", "О моде" },
            { "about.sdks", "NVIDIA DLSS SDK 310.9.16, NVIDIA NVAPI, AMD FSR SDK 2.4.0 (MIT)." },
            { "about.trademarks", "NVIDIA, DLSS, Reflex, AMD и FSR — товарные знаки их владельцев. Мод не связан с NVIDIA и AMD." },

            // === Words ===
            { "state.off", "Выкл" },
            { "state.on", "Вкл" },
            { "mode.off", "Выкл" },
            { "mode.quality", "Качество" },
            { "mode.balanced", "Баланс" },
            { "mode.performance", "Производительность" },
            { "mode.ultra", "Ультра-производительность" },

            // === Console ===
            { "con.ready", "{0} готов, видеокарта {1}" },
            { "con.caps", "DLSS {0} · DLSS FG {1} · FSR {2} · FSR FG {3} · Reflex {4}" },
            { "con.upscaler", "Апскейлер: {0}" },
            { "con.fg", "Генерация кадров: {0}" },
            { "con.reflex", "Reflex: {0}" },
            { "con.sharpness", "Резкость: {0}" },
            { "con.verbose", "Подробный лог: {0}" },
            { "con.plugin", "Нативный плагин не запустился: {0}" },
            { "con.no_device", "Нет устройства Direct3D 11: мод выключен." },
        };
    }
}
