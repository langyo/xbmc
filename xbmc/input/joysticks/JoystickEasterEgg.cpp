/*
 *  Copyright (C) 2016-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JoystickEasterEgg.h"

#include "ServiceBroker.h"
#include "games/GameServices.h"
#include "games/GameSettings.h"
#include "games/controllers/ControllerIDs.h"
#include "games/controllers/DefaultController.h"
#include "guilib/GUIAudioManager.h"
#include "guilib/WindowIDs.h"

using namespace KODI;
using namespace JOYSTICK;

const std::map<std::string, std::vector<FeatureName>> CJoystickEasterEgg::m_sequence = {
    {
        GAME::DEFAULT_CONTROLLER_ID,
        {
            GAME::CDefaultController::FEATURE_UP,
            GAME::CDefaultController::FEATURE_UP,
            GAME::CDefaultController::FEATURE_DOWN,
            GAME::CDefaultController::FEATURE_DOWN,
            GAME::CDefaultController::FEATURE_LEFT,
            GAME::CDefaultController::FEATURE_RIGHT,
            GAME::CDefaultController::FEATURE_LEFT,
            GAME::CDefaultController::FEATURE_RIGHT,
            GAME::CDefaultController::FEATURE_B,
            GAME::CDefaultController::FEATURE_A,
        },
    },
    {
        GAME::DEFAULT_REMOTE_ID,
        {
            "up",
            "up",
            "down",
            "down",
            "left",
            "right",
            "left",
            "right",
            "back",
            "ok",
        },
    },
};

CJoystickEasterEgg::CJoystickEasterEgg(const std::string& controllerId)
  : m_controllerId(controllerId)
{
}

bool CJoystickEasterEgg::OnButtonPress(const FeatureName& feature)
{
  bool bHandled = false;

  auto it = m_sequence.find(m_controllerId);
  if (it != m_sequence.end())
  {
    const auto& sequence = it->second;

    // Reset state if it previously finished
    if (m_state >= sequence.size())
      m_state = 0;

    if (feature == sequence[m_state])
      m_state++;
    else
      m_state = 0;

    if (IsCapturing())
    {
      bHandled = true;

      if (m_state >= sequence.size())
        OnFinish();
    }
  }

  return bHandled;
}

bool CJoystickEasterEgg::IsCapturing()
{
  // Capture input when finished with arrows (2 x up/down/left/right)
  return m_state > 8;
}

void CJoystickEasterEgg::OnFinish(void)
{
  GAME::CGameSettings& gameSettings = CServiceBroker::GetGameServices().GameSettings();
  gameSettings.ToggleGames();

  WINDOW_SOUND sound = gameSettings.GamesEnabled() ? SOUND_INIT : SOUND_DEINIT;
  CGUIComponent* gui = CServiceBroker::GetGUI();
  if (gui)
    gui->GetAudioManager().PlayWindowSound(WINDOW_DIALOG_KAI_TOAST, sound);

  //! @todo Shake screen
}
