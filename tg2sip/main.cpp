/*
 * Copyright (C) 2017-2018 infactum (infactum@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <https://www.gnu.org/licenses/>.
 */

#include <thread>
#include <optional>
#include <iostream>
#include <csignal>
#include <variant>
#include "logging.h"
#include "queue.h"
#include "utils.h"
#include "tg.h"
#include "sip.h"
#include "gateway.h"

int main() {
    pthread_setname_np(pthread_self(), "main");

     bool standard_folder = false;

    std::cout << "Loading config\n";

    if (const char* env_p = std::getenv("TG2SIP_STANDARD_FOLDER")) {
        std::cout << "Env.TG2SIP_STANDARD_FOLDER exist\n";
        std::string tddir( env_p );
         if (tddir.compare("YES") == 0) {
             std::cout << "Using standard folder\n";
             standard_folder = true;
         }
    }

    std::string config("settings.ini");

    if (standard_folder){
        config = "/etc/tg2sip/settings.ini";
    }

    auto reader = INIReader(config);
    Settings settings(reader, standard_folder);

    if (!settings.is_loaded()) {
        return 1;
    }

    init_logging(settings);

    std::set_terminate([]() {
        auto exc = std::current_exception();
        try {
            rethrow_exception(exc);
        } catch (std::exception &e) {
            spdlog::get("core")->critical("Unhandled exception: {}", e.what());
        }
        spdlog::drop_all();
        std::abort();
    });

    auto logger = spdlog::get("core");

    OptionalQueue<sip::events::Event> sip_events;
    /* will be removed on pj endpoint destroy */
    auto sip_log_writer = new sip::LogWriter(spdlog::get("pjsip"));
    auto sip_account = std::make_unique<sip::Account>(logger);
    auto sip_account_cfg = std::make_unique<sip::AccountConfig>(settings);
    auto sip_client = std::make_unique<sip::Client>(
            std::move(sip_account),
            std::move(sip_account_cfg),
            sip_events,
            settings,
            logger,
            sip_log_writer
    );
    sip_client->start();

    OptionalQueue<tg::Client::Object> tg_events;
    auto tg_client = std::make_unique<tg::Client>(settings, logger, tg_events);
    tg_client->start();
    auto tg_is_ready_future = tg_client->is_ready();
    auto tg_status = tg_is_ready_future.wait_for(std::chrono::seconds(5));
    if (tg_status != std::future_status::ready || !tg_is_ready_future.get()) {
        logger->critical("failed to start TG client");
        return 1;
    }

    auto gateway = std::make_unique<Gateway>(*sip_client, *tg_client, sip_events, tg_events, logger, settings);

    gateway->start();

    logger->info("performing a graceful shutdown...");
    spdlog::drop_all();

    return 0;
}
