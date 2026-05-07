/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "editor.h"

#include "auto_map.h"
#include "editor_actions.h"

#include <base/color.h>
#include <base/log.h>
#include <base/system.h>

#include <engine/client.h>
#include <engine/engine.h>
#include <engine/font_icons.h>
#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <generated/client_data.h>

#include <game/client/components/camera.h>
#include <game/client/gameclient.h>
#include <game/client/lineinput.h>
#include <game/client/ui.h>
#include <game/client/ui_listbox.h>
#include <game/client/ui_scrollregion.h>
#include <game/editor/editor_history.h>
#include <game/editor/explanations.h>
#include <game/editor/mapitems/image.h>
#include <game/editor/mapitems/sound.h>
#include <game/localization.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

static void DrawFilledCircle(IGraphics *pGraphics, float cx, float cy, float r)
{
	const int Sides = 16;
	for(int i = 0; i < Sides; i += 2)
	{
		const float a0 = 2.0f * pi * (float)i / (float)Sides;
		const float a1 = 2.0f * pi * (float)(i + 1) / (float)Sides;
		const float a2 = 2.0f * pi * (float)(i + 2) / (float)Sides;
		IGraphics::CFreeformItem ff(
			cx, cy,
			cx + std::cos(a0) * r, cy + std::sin(a0) * r,
			cx + std::cos(a2) * r, cy + std::sin(a2) * r,
			cx + std::cos(a1) * r, cy + std::sin(a1) * r);
		pGraphics->QuadsDrawFreeform(&ff, 1);
	}
}

static const char *VANILLA_IMAGES[] = {
	"bg_cloud1",
	"bg_cloud2",
	"bg_cloud3",
	"desert_doodads",
	"desert_main",
	"desert_mountains",
	"desert_mountains2",
	"desert_sun",
	"generic_deathtiles",
	"generic_unhookable",
	"grass_doodads",
	"grass_main",
	"jungle_background",
	"jungle_deathtiles",
	"jungle_doodads",
	"jungle_main",
	"jungle_midground",
	"jungle_unhookables",
	"moon",
	"mountains",
	"snow",
	"stars",
	"sun",
	"winter_doodads",
	"winter_main",
	"winter_mountains",
	"winter_mountains2",
	"winter_mountains3"};

bool CEditor::IsVanillaImage(const char *pImage)
{
	return std::any_of(std::begin(VANILLA_IMAGES), std::end(VANILLA_IMAGES), [pImage](const char *pVanillaImage) { return str_comp(pImage, pVanillaImage) == 0; });
}

void CEditor::EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels)
{
	if(EnvelopeIndex < 0 || EnvelopeIndex >= (int)Map()->m_vpEnvelopes.size())
		return;

	std::shared_ptr<CEnvelope> pEnvelope = Map()->m_vpEnvelopes[EnvelopeIndex];
	float Time = m_AnimateTime;
	Time *= m_AnimateSpeed;
	Time += (TimeOffsetMillis / 1000.0f);
	pEnvelope->Eval(Time, Result, Channels);
}

bool CEditor::CallbackOpenMap(const char *pFilename, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;
	if(pEditor->Load(pFilename, StorageType))
	{
		pEditor->Map()->m_ValidSaveFilename = StorageType == IStorage::TYPE_SAVE && pEditor->m_FileBrowser.IsValidSaveFilename();
		if(pEditor->m_Dialog == DIALOG_FILE)
		{
			pEditor->OnDialogClose();
		}
		return true;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to load map from file '%s'.", pFilename);
		return false;
	}
}

bool CEditor::CallbackAppendMap(const char *pFilename, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;
	const auto &&ErrorHandler = [pEditor](const char *pErrorMessage) {
		pEditor->ShowFileDialogError("%s", pErrorMessage);
		log_error("editor/append", "%s", pErrorMessage);
	};
	if(pEditor->Map()->Append(pFilename, StorageType, false, ErrorHandler))
	{
		pEditor->OnDialogClose();
		return true;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to load map from file '%s'.", pFilename);
		return false;
	}
}

bool CEditor::CallbackSaveMap(const char *pFilename, int StorageType, void *pUser)
{
	dbg_assert(StorageType == IStorage::TYPE_SAVE, "Saving only allowed for IStorage::TYPE_SAVE");

	CEditor *pEditor = static_cast<CEditor *>(pUser);

	// Save map to specified file
	if(pEditor->Save(pFilename))
	{
		if(pEditor->Map()->m_aFilename != pFilename)
		{
			str_copy(pEditor->Map()->m_aFilename, pFilename);
		}
		pEditor->Map()->m_ValidSaveFilename = true;
		pEditor->Map()->m_Modified = false;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to save map to file '%s'.", pFilename);
		return false;
	}

	// Also update autosave if it's older than half the configured autosave interval, so we also have periodic backups.
	const float Time = pEditor->Client()->GlobalTime();
	if(g_Config.m_EdAutosaveInterval > 0 && pEditor->Map()->m_LastSaveTime < Time && Time - pEditor->Map()->m_LastSaveTime > 30 * g_Config.m_EdAutosaveInterval)
	{
		const auto &&ErrorHandler = [pEditor](const char *pErrorMessage) {
			pEditor->ShowFileDialogError("%s", pErrorMessage);
			log_error("editor/autosave", "%s", pErrorMessage);
		};
		if(!pEditor->Map()->PerformAutosave(ErrorHandler))
			return false;
	}

	pEditor->OnDialogClose();
	return true;
}

bool CEditor::CallbackSaveCopyMap(const char *pFilename, int StorageType, void *pUser)
{
	dbg_assert(StorageType == IStorage::TYPE_SAVE, "Saving only allowed for IStorage::TYPE_SAVE");

	CEditor *pEditor = static_cast<CEditor *>(pUser);

	if(pEditor->Save(pFilename))
	{
		pEditor->OnDialogClose();
		return true;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to save map to file '%s'.", pFilename);
		return false;
	}
}

bool CEditor::CallbackSaveImage(const char *pFilename, int StorageType, void *pUser)
{
	dbg_assert(StorageType == IStorage::TYPE_SAVE, "Saving only allowed for IStorage::TYPE_SAVE");

	CEditor *pEditor = static_cast<CEditor *>(pUser);

	std::shared_ptr<CEditorImage> pImg = pEditor->Map()->SelectedImage();

	if(CImageLoader::SavePng(pEditor->Storage()->OpenFile(pFilename, IOFLAG_WRITE, StorageType), pFilename, *pImg))
	{
		pEditor->OnDialogClose();
		return true;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to write image to file '%s'.", pFilename);
		return false;
	}
}

bool CEditor::CallbackSaveSound(const char *pFilename, int StorageType, void *pUser)
{
	dbg_assert(StorageType == IStorage::TYPE_SAVE, "Saving only allowed for IStorage::TYPE_SAVE");

	CEditor *pEditor = static_cast<CEditor *>(pUser);

	std::shared_ptr<CEditorSound> pSound = pEditor->Map()->SelectedSound();

	IOHANDLE File = pEditor->Storage()->OpenFile(pFilename, IOFLAG_WRITE, StorageType);
	if(File)
	{
		io_write(File, pSound->m_pData, pSound->m_DataSize);
		io_close(File);
		pEditor->OnDialogClose();
		return true;
	}
	pEditor->ShowFileDialogError("Failed to open file '%s'.", pFilename);
	return false;
}

bool CEditor::CallbackCustomEntities(const char *pFilename, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;

	char aBuf[IO_MAX_PATH_LENGTH];
	IStorage::StripPathAndExtension(pFilename, aBuf, sizeof(aBuf));

	if(std::find(pEditor->m_vSelectEntitiesFiles.begin(), pEditor->m_vSelectEntitiesFiles.end(), std::string(aBuf)) != pEditor->m_vSelectEntitiesFiles.end())
	{
		pEditor->ShowFileDialogError("Custom entities cannot have the same name as default entities.");
		return false;
	}

	CImageInfo ImgInfo;
	if(!pEditor->Graphics()->LoadPng(ImgInfo, pFilename, StorageType))
	{
		pEditor->ShowFileDialogError("Failed to load image from file '%s'.", pFilename);
		return false;
	}

	pEditor->m_SelectEntitiesImage = aBuf;
	pEditor->m_AllowPlaceUnusedTiles = EUnusedEntities::ALLOWED_IMPLICIT;
	pEditor->m_PreventUnusedTilesWasWarned = false;

	pEditor->Graphics()->UnloadTexture(&pEditor->m_EntitiesTexture);
	pEditor->m_EntitiesTexture = pEditor->Graphics()->LoadTextureRawMove(ImgInfo, pEditor->GetTextureUsageFlag());

	pEditor->OnDialogClose();
	return true;
}

void CEditor::DoAudioPreview(CUIRect View, const void *pPlayPauseButtonId, const void *pStopButtonId, const void *pSeekBarId, int SampleId)
{
	CUIRect Button, SeekBar;
	// play/pause button
	{
		View.VSplitLeft(View.h, &Button, &View);
		if(DoButton_FontIcon(pPlayPauseButtonId, Sound()->IsPlaying(SampleId) ? FontIcon::PAUSE : FontIcon::PLAY, 0, &Button, BUTTONFLAG_LEFT, "Play/pause audio preview.", IGraphics::CORNER_ALL) ||
			(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_SPACE)))
		{
			if(Sound()->IsPlaying(SampleId))
			{
				Sound()->Pause(SampleId);
			}
			else
			{
				if(SampleId != m_ToolbarPreviewSound && m_ToolbarPreviewSound >= 0 && Sound()->IsPlaying(m_ToolbarPreviewSound))
					Sound()->Pause(m_ToolbarPreviewSound);

				Sound()->Play(CSounds::CHN_GUI, SampleId, ISound::FLAG_PREVIEW, 1.0f);
			}
		}
	}
	// stop button
	{
		View.VSplitLeft(2.0f, nullptr, &View);
		View.VSplitLeft(View.h, &Button, &View);
		if(DoButton_FontIcon(pStopButtonId, FontIcon::STOP, 0, &Button, BUTTONFLAG_LEFT, "Stop audio preview.", IGraphics::CORNER_ALL))
		{
			Sound()->Stop(SampleId);
		}
	}
	// do seekbar
	{
		View.VSplitLeft(5.0f, nullptr, &View);
		const float Cut = std::min(View.w, 200.0f);
		View.VSplitLeft(Cut, &SeekBar, &View);
		SeekBar.HMargin(2.5f, &SeekBar);

		const float Rounding = 5.0f;

		char aBuffer[64];
		const float CurrentTime = Sound()->GetSampleCurrentTime(SampleId);
		const float TotalTime = Sound()->GetSampleTotalTime(SampleId);

		// draw seek bar
		SeekBar.Draw(ColorRGBA(0, 0, 0, 0.5f), IGraphics::CORNER_ALL, Rounding);

		// draw filled bar
		const float Amount = CurrentTime / TotalTime;
		CUIRect FilledBar = SeekBar;
		FilledBar.w = 2 * Rounding + (FilledBar.w - 2 * Rounding) * Amount;
		FilledBar.Draw(ColorRGBA(1, 1, 1, 0.5f), IGraphics::CORNER_ALL, Rounding);

		// draw time
		char aCurrentTime[32];
		str_time_float(CurrentTime, ETimeFormat::HOURS, aCurrentTime, sizeof(aCurrentTime));
		char aTotalTime[32];
		str_time_float(TotalTime, ETimeFormat::HOURS, aTotalTime, sizeof(aTotalTime));
		str_format(aBuffer, sizeof(aBuffer), "%s / %s", aCurrentTime, aTotalTime);
		Ui()->DoLabel(&SeekBar, aBuffer, SeekBar.h * 0.70f, TEXTALIGN_MC);

		// do the logic
		const bool Inside = Ui()->MouseInside(&SeekBar);

		if(Ui()->CheckActiveItem(pSeekBarId))
		{
			if(!Ui()->MouseButton(0))
			{
				Ui()->SetActiveItem(nullptr);
			}
			else
			{
				const float AmountSeek = std::clamp((Ui()->MouseX() - SeekBar.x - Rounding) / (SeekBar.w - 2 * Rounding), 0.0f, 1.0f);
				Sound()->SetSampleCurrentTime(SampleId, AmountSeek);
			}
		}
		else if(Ui()->HotItem() == pSeekBarId)
		{
			if(Ui()->MouseButton(0))
				Ui()->SetActiveItem(pSeekBarId);
		}

		if(Inside && !Ui()->MouseButton(0))
			Ui()->SetHotItem(pSeekBarId);
	}
}

void CEditor::DoToolbarLayers(CUIRect ToolBar)
{
	const bool ModPressed = Input()->ModifierIsPressed();
	const bool ShiftPressed = Input()->ShiftIsPressed();

	// handle shortcut for info button
	if(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_I) && ModPressed && !ShiftPressed)
	{
		if(m_ShowTileInfo == SHOW_TILE_HEXADECIMAL)
			m_ShowTileInfo = SHOW_TILE_DECIMAL;
		else if(m_ShowTileInfo != SHOW_TILE_OFF)
			m_ShowTileInfo = SHOW_TILE_OFF;
		else
			m_ShowTileInfo = SHOW_TILE_DECIMAL;
	}

	// handle shortcut for hex button
	if(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_I) && ModPressed && ShiftPressed)
	{
		m_ShowTileInfo = m_ShowTileInfo == SHOW_TILE_HEXADECIMAL ? SHOW_TILE_OFF : SHOW_TILE_HEXADECIMAL;
	}

	// handle shortcut for unused button
	if(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_U) && ModPressed && m_AllowPlaceUnusedTiles != EUnusedEntities::ALLOWED_IMPLICIT)
	{
		if(m_AllowPlaceUnusedTiles == EUnusedEntities::ALLOWED_EXPLICIT)
		{
			m_AllowPlaceUnusedTiles = EUnusedEntities::NOT_ALLOWED;
		}
		else
		{
			m_AllowPlaceUnusedTiles = EUnusedEntities::ALLOWED_EXPLICIT;
		}
	}

	CUIRect ToolbarTop, ToolbarBottom;
	CUIRect Button;

	ToolBar.HSplitMid(&ToolbarTop, &ToolbarBottom, 5.0f);

	// top line buttons
	{
		// detail button
		ToolbarTop.VSplitLeft(40.0f, &Button, &ToolbarTop);
		static int s_HqButton = 0;
		if(DoButton_Editor(&s_HqButton, "HD", m_ShowDetail, &Button, BUTTONFLAG_LEFT, "[Ctrl+H] Toggle high detail.") ||
			(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_H) && ModPressed))
		{
			m_ShowDetail = !m_ShowDetail;
		}

		ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

		// animation button
		ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
		static char s_AnimateButton;
		if(DoButton_FontIcon(&s_AnimateButton, FontIcon::CIRCLE_PLAY, m_Animate, &Button, BUTTONFLAG_LEFT, "[Ctrl+M] Toggle animation.", IGraphics::CORNER_L) ||
			(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_M) && ModPressed))
		{
			m_AnimateStart = time_get();
			m_Animate = !m_Animate;
		}

		// animation settings button
		ToolbarTop.VSplitLeft(14.0f, &Button, &ToolbarTop);
		static char s_AnimateSettingsButton;
		if(DoButton_FontIcon(&s_AnimateSettingsButton, FontIcon::CIRCLE_CHEVRON_DOWN, 0, &Button, BUTTONFLAG_LEFT, "Change the animation settings.", IGraphics::CORNER_R, 8.0f))
		{
			m_AnimateUpdatePopup = true;
			static SPopupMenuId s_PopupAnimateSettingsId;
			Ui()->DoPopupMenu(&s_PopupAnimateSettingsId, Button.x, Button.y + Button.h, 150.0f, 37.0f, this, PopupAnimateSettings);
		}

		ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

		// proof button
		ToolbarTop.VSplitLeft(40.0f, &Button, &ToolbarTop);
		if(DoButton_Ex(&m_QuickActionProof, m_QuickActionProof.Label(), m_QuickActionProof.Active(), &Button, BUTTONFLAG_LEFT, m_QuickActionProof.Description(), IGraphics::CORNER_L))
		{
			m_QuickActionProof.Call();
		}

		ToolbarTop.VSplitLeft(14.0f, &Button, &ToolbarTop);
		static int s_ProofModeButton = 0;
		if(DoButton_FontIcon(&s_ProofModeButton, FontIcon::CIRCLE_CHEVRON_DOWN, 0, &Button, BUTTONFLAG_LEFT, "Select proof mode.", IGraphics::CORNER_R, 8.0f))
		{
			static SPopupMenuId s_PopupProofModeId;
			Ui()->DoPopupMenu(&s_PopupProofModeId, Button.x, Button.y + Button.h, 60.0f, 36.0f, this, PopupProofMode);
		}

		ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

		// zoom button
		ToolbarTop.VSplitLeft(40.0f, &Button, &ToolbarTop);
		static int s_ZoomButton = 0;
		if(DoButton_Editor(&s_ZoomButton, "Zoom", m_PreviewZoom, &Button, BUTTONFLAG_LEFT, "Toggle preview of how layers will be zoomed ingame."))
		{
			m_PreviewZoom = !m_PreviewZoom;
		}

		ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

		// grid button
		ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
		static int s_GridButton = 0;
		if(DoButton_FontIcon(&s_GridButton, FontIcon::BORDER_ALL, m_QuickActionToggleGrid.Active(), &Button, BUTTONFLAG_LEFT, m_QuickActionToggleGrid.Description(), IGraphics::CORNER_L) ||
			(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_G) && ModPressed && !ShiftPressed))
		{
			m_QuickActionToggleGrid.Call();
		}

		// grid settings button
		ToolbarTop.VSplitLeft(14.0f, &Button, &ToolbarTop);
		static char s_GridSettingsButton;
		if(DoButton_FontIcon(&s_GridSettingsButton, FontIcon::CIRCLE_CHEVRON_DOWN, 0, &Button, BUTTONFLAG_LEFT, "Change the grid settings.", IGraphics::CORNER_R, 8.0f))
		{
			MapView()->MapGrid()->DoSettingsPopup(vec2(Button.x, Button.y + Button.h));
		}

		ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

		// zoom group
		ToolbarTop.VSplitLeft(20.0f, &Button, &ToolbarTop);
		static int s_ZoomOutButton = 0;
		if(DoButton_FontIcon(&s_ZoomOutButton, FontIcon::MINUS, 0, &Button, BUTTONFLAG_LEFT, m_QuickActionZoomOut.Description(), IGraphics::CORNER_L))
		{
			m_QuickActionZoomOut.Call();
		}

		ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
		static int s_ZoomNormalButton = 0;
		if(DoButton_FontIcon(&s_ZoomNormalButton, FontIcon::MAGNIFYING_GLASS, 0, &Button, BUTTONFLAG_LEFT, m_QuickActionResetZoom.Description(), IGraphics::CORNER_NONE))
		{
			m_QuickActionResetZoom.Call();
		}

		ToolbarTop.VSplitLeft(20.0f, &Button, &ToolbarTop);
		static int s_ZoomInButton = 0;
		if(DoButton_FontIcon(&s_ZoomInButton, FontIcon::PLUS, 0, &Button, BUTTONFLAG_LEFT, m_QuickActionZoomIn.Description(), IGraphics::CORNER_R))
		{
			m_QuickActionZoomIn.Call();
		}

		ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

		// undo/redo group
		ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
		static int s_UndoButton = 0;
		if(DoButton_FontIcon(&s_UndoButton, FontIcon::UNDO, Map()->m_EditorHistory.CanUndo() - 1, &Button, BUTTONFLAG_LEFT, "[Ctrl+Z] Undo the last action.", IGraphics::CORNER_L))
		{
			Map()->m_EditorHistory.Undo();
		}

		ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
		static int s_RedoButton = 0;
		if(DoButton_FontIcon(&s_RedoButton, FontIcon::REDO, Map()->m_EditorHistory.CanRedo() - 1, &Button, BUTTONFLAG_LEFT, "[Ctrl+Y] Redo the last action.", IGraphics::CORNER_R))
		{
			Map()->m_EditorHistory.Redo();
		}

		ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

		// brush manipulation
		{
			int Enabled = m_pBrush->IsEmpty() ? -1 : 0;

			// flip buttons
			ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
			static int s_FlipXButton = 0;
			if(DoButton_FontIcon(&s_FlipXButton, FontIcon::ARROWS_LEFT_RIGHT, Enabled, &Button, BUTTONFLAG_LEFT, "[N] Flip the brush horizontally.", IGraphics::CORNER_L) || (Input()->KeyPress(KEY_N) && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && !Ui()->IsPopupOpen()))
			{
				for(auto &pLayer : m_pBrush->m_vpLayers)
					pLayer->BrushFlipX();
			}

			ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
			static int s_FlipyButton = 0;
			if(DoButton_FontIcon(&s_FlipyButton, FontIcon::ARROWS_UP_DOWN, Enabled, &Button, BUTTONFLAG_LEFT, "[M] Flip the brush vertically.", IGraphics::CORNER_R) || (Input()->KeyPress(KEY_M) && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && !Ui()->IsPopupOpen()))
			{
				for(auto &pLayer : m_pBrush->m_vpLayers)
					pLayer->BrushFlipY();
			}
			ToolbarTop.VSplitLeft(5.0f, nullptr, &ToolbarTop);

			// rotate buttons
			ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
			static int s_RotationAmount = 90;
			bool TileLayer = false;
			// check for tile layers in brush selection
			for(auto &pLayer : m_pBrush->m_vpLayers)
				if(pLayer->m_Type == LAYERTYPE_TILES)
				{
					TileLayer = true;
					// In select mode allow arbitrary angle; otherwise snap to 90° for tile layers
					if(!m_SelectTool)
						s_RotationAmount = maximum(90, (s_RotationAmount / 90) * 90);
					break;
				}

			static int s_CcwButton = 0;
			if(DoButton_FontIcon(&s_CcwButton, FontIcon::ARROW_ROTATE_LEFT, Enabled, &Button, BUTTONFLAG_LEFT, "[R] Rotate the brush counter-clockwise.", IGraphics::CORNER_L) || (Input()->KeyPress(KEY_R) && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && !Ui()->IsPopupOpen()))
			{
				if(m_SelectTool && !m_vSelectOrigTiles.empty())
				{
					m_SelectTotalAngle -= s_RotationAmount / 360.0f * pi * 2.0f;
					for(auto &pLayer : m_pBrush->m_vpLayers)
						if(pLayer->m_Type == LAYERTYPE_TILES)
						{
							std::static_pointer_cast<CLayerTiles>(pLayer)->BrushRotateArbitrary(m_vSelectOrigTiles, m_SelectOrigW, m_SelectOrigH, m_SelectTotalAngle);
							break;
						}
				}
				else
				{
					for(auto &pLayer : m_pBrush->m_vpLayers)
						pLayer->BrushRotate(-s_RotationAmount / 360.0f * pi * 2);
				}
			}

			ToolbarTop.VSplitLeft(30.0f, &Button, &ToolbarTop);
			auto RotationAmountRes = UiDoValueSelector(&s_RotationAmount, &Button, "", s_RotationAmount, (TileLayer && !m_SelectTool) ? 90 : 1, 359, (TileLayer && !m_SelectTool) ? 90 : 1, (TileLayer && !m_SelectTool) ? 10.0f : 2.0f, "Rotation of the brush in degrees. Use left mouse button to drag and change the value. Hold shift to be more precise.", true, false, IGraphics::CORNER_NONE);
			s_RotationAmount = RotationAmountRes.m_Value;

			ToolbarTop.VSplitLeft(25.0f, &Button, &ToolbarTop);
			static int s_CwButton = 0;
			if(DoButton_FontIcon(&s_CwButton, FontIcon::ARROW_ROTATE_RIGHT, Enabled, &Button, BUTTONFLAG_LEFT, "[T] Rotate the brush clockwise.", IGraphics::CORNER_R) || (Input()->KeyPress(KEY_T) && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && !Ui()->IsPopupOpen()))
			{
				if(m_SelectTool && !m_vSelectOrigTiles.empty())
				{
					m_SelectTotalAngle += s_RotationAmount / 360.0f * pi * 2.0f;
					for(auto &pLayer : m_pBrush->m_vpLayers)
						if(pLayer->m_Type == LAYERTYPE_TILES)
						{
							std::static_pointer_cast<CLayerTiles>(pLayer)->BrushRotateArbitrary(m_vSelectOrigTiles, m_SelectOrigW, m_SelectOrigH, m_SelectTotalAngle);
							break;
						}
				}
				else
				{
					for(auto &pLayer : m_pBrush->m_vpLayers)
						pLayer->BrushRotate(s_RotationAmount / 360.0f * pi * 2);
				}
			}
		}

		// Color pipette and palette
		{
			const float PipetteButtonWidth = 30.0f;
			const float ColorPickerButtonWidth = 20.0f;
			const float Spacing = 2.0f;
			const size_t NumColorsShown = std::clamp<int>(round_to_int((ToolbarTop.w - PipetteButtonWidth - 40.0f) / (ColorPickerButtonWidth + Spacing)), 1, std::size(m_aSavedColors));

			CUIRect ColorPalette;
			ToolbarTop.VSplitRight(NumColorsShown * (ColorPickerButtonWidth + Spacing) + PipetteButtonWidth, &ToolbarTop, &ColorPalette);

			// Pipette button
			static char s_PipetteButton;
			ColorPalette.VSplitLeft(PipetteButtonWidth, &Button, &ColorPalette);
			ColorPalette.VSplitLeft(Spacing, nullptr, &ColorPalette);
			if(DoButton_FontIcon(&s_PipetteButton, FontIcon::EYE_DROPPER, m_QuickActionPipette.Active(), &Button, BUTTONFLAG_LEFT, m_QuickActionPipette.Description(), IGraphics::CORNER_ALL) ||
				(CLineInput::GetActiveInput() == nullptr && ModPressed && ShiftPressed && Input()->KeyPress(KEY_C)))
			{
				m_QuickActionPipette.Call();
			}

			// Palette color pickers
			for(size_t i = 0; i < NumColorsShown; ++i)
			{
				ColorPalette.VSplitLeft(ColorPickerButtonWidth, &Button, &ColorPalette);
				ColorPalette.VSplitLeft(Spacing, nullptr, &ColorPalette);
				const auto &&SetColor = [&](ColorRGBA NewColor) {
					m_aSavedColors[i] = NewColor;
				};
				DoColorPickerButton(&m_aSavedColors[i], &Button, m_aSavedColors[i], SetColor);
			}
		}
	}

	// Bottom line buttons
	{
		// refocus button
		{
			ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
			int FocusButtonChecked = MapView()->IsFocused() ? -1 : 1;
			if(DoButton_Editor(&m_QuickActionRefocus, m_QuickActionRefocus.Label(), FocusButtonChecked, &Button, BUTTONFLAG_LEFT, m_QuickActionRefocus.Description()) || (m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_HOME)))
				m_QuickActionRefocus.Call();
			ToolbarBottom.VSplitLeft(5.0f, nullptr, &ToolbarBottom);
		}

		// tile manipulation
		{
			// do tele/tune/switch/speedup button
			{
				std::shared_ptr<CLayerTiles> pS = std::static_pointer_cast<CLayerTiles>(Map()->SelectedLayerType(0, LAYERTYPE_TILES));
				if(pS)
				{
					const char *pButtonName = nullptr;
					CUi::FPopupMenuFunction pfnPopupFunc = nullptr;
					int Rows = 0;
					int ExtraWidth = 0;
					if(pS == Map()->m_pSwitchLayer)
					{
						pButtonName = "Switch";
						pfnPopupFunc = PopupSwitch;
						Rows = 3;
					}
					else if(pS == Map()->m_pSpeedupLayer)
					{
						pButtonName = "Speedup";
						pfnPopupFunc = PopupSpeedup;
						Rows = 3;
					}
					else if(pS == Map()->m_pTuneLayer)
					{
						pButtonName = "Tune";
						pfnPopupFunc = PopupTune;
						Rows = 2;
					}
					else if(pS == Map()->m_pTeleLayer)
					{
						pButtonName = "Tele";
						pfnPopupFunc = PopupTele;
						Rows = 3;
						ExtraWidth = 50;
					}

					if(pButtonName != nullptr)
					{
						static char s_aButtonTooltip[64];
						str_format(s_aButtonTooltip, sizeof(s_aButtonTooltip), "[Ctrl+T] %s", pButtonName);

						ToolbarBottom.VSplitLeft(60.0f, &Button, &ToolbarBottom);
						static int s_ModifierButton = 0;
						if(DoButton_Ex(&s_ModifierButton, pButtonName, 0, &Button, BUTTONFLAG_LEFT, s_aButtonTooltip, IGraphics::CORNER_ALL) || (m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && ModPressed && Input()->KeyPress(KEY_T)))
						{
							static SPopupMenuId s_PopupModifierId;
							if(!Ui()->IsPopupOpen(&s_PopupModifierId))
							{
								Ui()->DoPopupMenu(&s_PopupModifierId, Button.x, Button.y + Button.h, 120 + ExtraWidth, 10.0f + Rows * 13.0f, this, pfnPopupFunc);
							}
						}
						ToolbarBottom.VSplitLeft(5.0f, nullptr, &ToolbarBottom);
					}
				}
			}
		}

		// do add quad/sound button
		std::shared_ptr<CLayer> pLayer = Map()->SelectedLayer(0);
		if(pLayer && (pLayer->m_Type == LAYERTYPE_QUADS || pLayer->m_Type == LAYERTYPE_SOUNDS))
		{
			// "Add sound source" button needs more space or the font size will be scaled down
			ToolbarBottom.VSplitLeft((pLayer->m_Type == LAYERTYPE_QUADS) ? 60.0f : 100.0f, &Button, &ToolbarBottom);

			if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				if(DoButton_Editor(&m_QuickActionAddQuad, m_QuickActionAddQuad.Label(), 0, &Button, BUTTONFLAG_LEFT, m_QuickActionAddQuad.Description()) ||
					(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_Q) && ModPressed))
				{
					m_QuickActionAddQuad.Call();
				}
			}
			else if(pLayer->m_Type == LAYERTYPE_SOUNDS)
			{
				if(DoButton_Editor(&m_QuickActionAddSoundSource, m_QuickActionAddSoundSource.Label(), 0, &Button, BUTTONFLAG_LEFT, m_QuickActionAddSoundSource.Description()) ||
					(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_Q) && ModPressed))
				{
					m_QuickActionAddSoundSource.Call();
				}
			}

			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		// Brush draw mode button
		{
			ToolbarBottom.VSplitLeft(65.0f, &Button, &ToolbarBottom);
			static int s_BrushDrawModeButton = 0;
			if(DoButton_Editor(&s_BrushDrawModeButton, "Destructive", m_BrushDrawDestructive, &Button, BUTTONFLAG_LEFT, "[Ctrl+D] Toggle brush draw mode: preserve or override existing tiles.") ||
				(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_D) && ModPressed && !ShiftPressed))
				m_BrushDrawDestructive = !m_BrushDrawDestructive;
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		// Select tool toggle
		{
			ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
			static int s_SelectToolButton = 0;
			if(DoButton_Editor(&s_SelectToolButton, "Select", m_SelectTool, &Button, BUTTONFLAG_LEFT, "Toggle select tool: grab a rectangle or freehand selection and rotate it at any angle with [R]/[T]."))
			{
				m_SelectTool = !m_SelectTool;
				if(m_SelectTool)
				{
					m_FillTool = false;
					m_ShapeTool = false;
					if(m_LineTool)
						LineToolCancel();
					m_LineTool = false;
					if(m_FadeTool)
						FadeToolCancel();
					m_FadeTool = false;
					ThreeDToolCancel();
					m_ThreeDTool = false;
				}
				else
					SelectToolCancelOrig();
			}
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		if(m_SelectTool)
		{
			ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
			static int s_SelectRectButton = 0;
			if(DoButton_Ex(&s_SelectRectButton, "Rect", !m_SelectFreehand, &Button, BUTTONFLAG_LEFT, "Rectangle selection mode.", IGraphics::CORNER_L))
				m_SelectFreehand = false;
			ToolbarBottom.VSplitLeft(60.0f, &Button, &ToolbarBottom);
			static int s_SelectFreeButton = 0;
			if(DoButton_Ex(&s_SelectFreeButton, "Freehand", m_SelectFreehand, &Button, BUTTONFLAG_LEFT, "Freehand (lasso) selection mode: drag to draw a polygon.", IGraphics::CORNER_R))
				m_SelectFreehand = true;
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		// 3D tool toggle
		{
			ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
			static int s_ThreeDToolButton = 0;
			if(DoButton_Editor(&s_ThreeDToolButton, "3D", m_ThreeDTool, &Button, BUTTONFLAG_LEFT, "Toggle 3D perspective tool: draws 3D shapes projected to tiles. Drag nodes to scale/rotate/move. Enter to stamp, Esc to cancel. Brush must be a single tile."))
			{
				m_ThreeDTool = !m_ThreeDTool;
				if(m_ThreeDTool)
				{
					m_FillTool = false;
					m_ShapeTool = false;
					if(m_LineTool)
						LineToolCancel();
					m_LineTool = false;
					m_FadeTool = false;
					FadeToolCancel();
					m_SelectTool = false;
					SelectToolCancelOrig();
					ThreeDToolEnsureInit();
				}
				else
					ThreeDToolCancel();
			}
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		// Fill tool toggle
		{
			ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
			static int s_FillToolButton = 0;
			if(DoButton_Editor(&s_FillToolButton, "Fill", m_FillTool, &Button, BUTTONFLAG_LEFT, "Toggle fill tool: left-click flood-fills connected tiles of the same index (4-connected, diagonals are borders)."))
			{
				m_FillTool = !m_FillTool;
				if(m_FillTool)
				{
					m_ShapeTool = false;
					if(m_LineTool)
						LineToolCancel();
					m_LineTool = false;
					if(m_FadeTool)
						FadeToolCancel();
					m_FadeTool = false;
					m_SelectTool = false;
					SelectToolCancelOrig();
					ThreeDToolCancel();
					m_ThreeDTool = false;
				}
			}
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		// Fill X/Y shift selectors (only when Fill is on and brush is multi-tile)
		if(m_FillTool && !m_pBrush->IsEmpty())
		{
			int BrushW = 1, BrushH = 1;
			for(const auto &pLayer : m_pBrush->m_vpLayers)
			{
				if(pLayer->m_Type == LAYERTYPE_TILES)
				{
					std::shared_ptr<CLayerTiles> pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
					if(pTiles->m_Width * pTiles->m_Height > BrushW * BrushH)
					{
						BrushW = pTiles->m_Width;
						BrushH = pTiles->m_Height;
					}
				}
			}
			if(BrushW > 1 || BrushH > 1)
			{
				m_FillShiftX = ((m_FillShiftX % BrushW) + BrushW) % BrushW;
				m_FillShiftY = ((m_FillShiftY % BrushH) + BrushH) % BrushH;

				ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
				static int s_FillShiftXSelector = 0;
				auto ResX = UiDoValueSelector(&s_FillShiftXSelector, &Button, "X:", m_FillShiftX, 0, BrushW - 1, 1, 5.0f, "Fill brush X shift. Drag with left mouse to change.", false, false, IGraphics::CORNER_L);
				m_FillShiftX = ResX.m_Value;

				ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
				static int s_FillShiftYSelector = 0;
				auto ResY = UiDoValueSelector(&s_FillShiftYSelector, &Button, "Y:", m_FillShiftY, 0, BrushH - 1, 1, 5.0f, "Fill brush Y shift. Drag with left mouse to change.", false, false, IGraphics::CORNER_R);
				m_FillShiftY = ResY.m_Value;

				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}
		}

		// Shape tool toggle
		{
			ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
			static int s_ShapeToolButton = 0;
			if(DoButton_Editor(&s_ShapeToolButton, "Shape", m_ShapeTool, &Button, BUTTONFLAG_LEFT, "Toggle shape tool: click-drag to draw a rectangle or ellipse (Shift = perfect square/circle). Brush must be a single tile."))
			{
				m_ShapeTool = !m_ShapeTool;
				if(m_ShapeTool)
				{
					m_FillTool = false;
					if(m_LineTool)
						LineToolCancel();
					m_LineTool = false;
					if(m_FadeTool)
						FadeToolCancel();
					m_FadeTool = false;
					m_SelectTool = false;
					SelectToolCancelOrig();
					ThreeDToolCancel();
					m_ThreeDTool = false;
				}
				else
					ShapeGhostClear();
			}
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		if(m_ShapeTool)
		{
			// Shape kind picker: Rect / Ellipse / Triangle / N-gon
			{
				ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
				static int s_ShapeRectButton = 0;
				if(DoButton_Ex(&s_ShapeRectButton, "Rect", m_ShapeKind == SHAPE_RECT, &Button, BUTTONFLAG_LEFT, "Rectangle shape.", IGraphics::CORNER_L))
					m_ShapeKind = SHAPE_RECT;
				ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
				static int s_ShapeEllipseButton = 0;
				if(DoButton_Ex(&s_ShapeEllipseButton, "Ellipse", m_ShapeKind == SHAPE_ELLIPSE, &Button, BUTTONFLAG_LEFT, "Ellipse shape.", IGraphics::CORNER_NONE))
					m_ShapeKind = SHAPE_ELLIPSE;
				ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
				static int s_ShapeTriButton = 0;
				if(DoButton_Ex(&s_ShapeTriButton, "Tri", m_ShapeKind == SHAPE_TRIANGLE, &Button, BUTTONFLAG_LEFT, "Triangle (isoceles, fitted to bbox).", IGraphics::CORNER_NONE))
					m_ShapeKind = SHAPE_TRIANGLE;
				ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
				static int s_ShapeNgonButton = 0;
				if(DoButton_Ex(&s_ShapeNgonButton, "N-gon", m_ShapeKind == SHAPE_NGON, &Button, BUTTONFLAG_LEFT, "Regular polygon (set N below).", IGraphics::CORNER_R))
					m_ShapeKind = SHAPE_NGON;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);

				if(m_ShapeKind == SHAPE_NGON)
				{
					ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
					static int s_ShapeNgonSidesSelector = 0;
					auto ResN = UiDoValueSelector(&s_ShapeNgonSidesSelector, &Button, "N:", m_ShapeNgonSides, 3, 32, 1, 5.0f, "Number of polygon sides. Drag to change.");
					m_ShapeNgonSides = ResN.m_Value;
					ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
				}
			}

			// Filled toggle
			{
				ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
				static int s_ShapeFilledButton = 0;
				if(DoButton_Editor(&s_ShapeFilledButton, m_ShapeFilled ? "Filled" : "Outline", m_ShapeFilled, &Button, BUTTONFLAG_LEFT, "Toggle filled / outline."))
					m_ShapeFilled = !m_ShapeFilled;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}

			// Thickness selector (only for outlines)
			if(!m_ShapeFilled)
			{
				ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
				static int s_ShapeThicknessSelector = 0;
				auto Res = UiDoValueSelector(&s_ShapeThicknessSelector, &Button, "Thick:", m_ShapeThickness, 1, 64, 1, 5.0f, "Outline thickness in tiles. Drag to change.");
				m_ShapeThickness = Res.m_Value;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}
		}

		// Line tool toggle
		{
			ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
			static int s_LineToolButton = 0;
			if(DoButton_Editor(&s_LineToolButton, "Line", m_LineTool, &Button, BUTTONFLAG_LEFT, "Toggle line tool: click to add nodes, drag to reshape, click on a segment to insert a node, Enter to commit, Esc/right-click to cancel. Smooth bezier curve through nodes."))
			{
				if(m_LineTool)
					LineToolCancel();
				m_LineTool = !m_LineTool;
				if(m_LineTool)
				{
					m_FillTool = false;
					m_ShapeTool = false;
					if(m_FadeTool)
						FadeToolCancel();
					m_FadeTool = false;
					m_SelectTool = false;
					SelectToolCancelOrig();
					ThreeDToolCancel();
					m_ThreeDTool = false;
				}
			}
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		if(m_LineTool)
		{
			ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
			static int s_LineThicknessSelector = 0;
			auto Res = UiDoValueSelector(&s_LineThicknessSelector, &Button, "Thick:", m_LineThickness, 1, 64, 1, 5.0f, "Line thickness in tiles. Drag to change.");
			m_LineThickness = Res.m_Value;
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);

			ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
			static int s_LineSmoothButton = 0;
			if(DoButton_Ex(&s_LineSmoothButton, "Smooth", m_LineDefaultNodeKind == LINE_NODE_SMOOTH, &Button, BUTTONFLAG_LEFT, "Default kind for new nodes: smooth (bezier).", IGraphics::CORNER_L))
				m_LineDefaultNodeKind = LINE_NODE_SMOOTH;
			ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
			static int s_LineSharpButton = 0;
			if(DoButton_Ex(&s_LineSharpButton, "Sharp", m_LineDefaultNodeKind == LINE_NODE_SHARP, &Button, BUTTONFLAG_LEFT, "Default kind for new nodes: sharp (corner). Click an existing node to cycle.", IGraphics::CORNER_R))
				m_LineDefaultNodeKind = LINE_NODE_SHARP;
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);

			ToolbarBottom.VSplitLeft(35.0f, &Button, &ToolbarBottom);
			static int s_LineCRButton = 0;
			if(DoButton_Ex(&s_LineCRButton, "C-R", m_LineCatmullRom, &Button, BUTTONFLAG_LEFT, "Catmull-Rom spline mode: smooth curves through all nodes without bezier handles.", IGraphics::CORNER_ALL))
				m_LineCatmullRom = !m_LineCatmullRom;
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}



		// Fade tool toggle
		{
			ToolbarBottom.VSplitLeft(40.0f, &Button, &ToolbarBottom);
			static int s_FadeToolButton = 0;
			if(DoButton_Editor(&s_FadeToolButton, "Fade", m_FadeTool, &Button, BUTTONFLAG_LEFT, "Toggle fade/dither tool: a region appears on the canvas, drag corners/edges/center, adjust density. Enter commits, Esc cancels."))
			{
				if(m_FadeTool)
					FadeToolCancel();
				m_FadeTool = !m_FadeTool;
				if(m_FadeTool)
				{
					m_FillTool = false;
					m_ShapeTool = false;
					if(m_LineTool)
						LineToolCancel();
					m_LineTool = false;
					m_SelectTool = false;
					SelectToolCancelOrig();
					ThreeDToolCancel();
					m_ThreeDTool = false;
					FadeToolEnsureInit();
				}
			}
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}

		if(m_FadeTool)
		{
			// Width — start and end (taper for V-shapes)
			{
				ToolbarBottom.VSplitLeft(60.0f, &Button, &ToolbarBottom);
				static int s_FadeWidthSel = 0;
				auto RW = UiDoValueSelector(&s_FadeWidthSel, &Button, "Wid:", m_FadeWidth, 0, 64, 1, 5.0f, "Half-width of the path at the start node, in tiles.");
				m_FadeWidth = RW.m_Value;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);

				ToolbarBottom.VSplitLeft(60.0f, &Button, &ToolbarBottom);
				static int s_FadeWidthEndSel = 0;
				auto RWE = UiDoValueSelector(&s_FadeWidthEndSel, &Button, "WidE:", m_FadeWidthEnd, 0, 64, 1, 5.0f, "Half-width at the end node — different from Wid: makes a V-shaped tapered path.");
				m_FadeWidthEnd = RWE.m_Value;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}

			// Density start / end
			{
				ToolbarBottom.VSplitLeft(60.0f, &Button, &ToolbarBottom);
				static int s_FadeDStartSel = 0;
				int StartPct = (int)(m_FadeDensityStart * 100.0f + 0.5f);
				auto R0 = UiDoValueSelector(&s_FadeDStartSel, &Button, "S%:", StartPct, 0, 100, 1, 5.0f, "Density at start of fade (the 'sparse' end), in percent.");
				m_FadeDensityStart = R0.m_Value / 100.0f;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);

				ToolbarBottom.VSplitLeft(60.0f, &Button, &ToolbarBottom);
				static int s_FadeDEndSel = 0;
				int EndPct = (int)(m_FadeDensityEnd * 100.0f + 0.5f);
				auto R1 = UiDoValueSelector(&s_FadeDEndSel, &Button, "E%:", EndPct, 0, 100, 1, 5.0f, "Density at end of fade (the 'dense' end), in percent.");
				m_FadeDensityEnd = R1.m_Value / 100.0f;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}

			// Feather
			{
				ToolbarBottom.VSplitLeft(60.0f, &Button, &ToolbarBottom);
				static int s_FadeFeatherSel = 0;
				auto RF = UiDoValueSelector(&s_FadeFeatherSel, &Button, "Soft:", m_FadeFeather, 0, 64, 1, 5.0f, "Edge softness in tiles: dither extends this far beyond the rect with falloff. 0 = sharp.");
				m_FadeFeather = RF.m_Value;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}

			// Randomize button
			{
				ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
				static int s_FadeRandomizeBtn = 0;
				if(DoButton_Editor(&s_FadeRandomizeBtn, "Random", 0, &Button, BUTTONFLAG_LEFT, "Re-roll the dither seed."))
					m_FadeSeed = (int)(time_get() & 0x7FFFFFFF);
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}

			// Air toggle
			{
				ToolbarBottom.VSplitLeft(50.0f, &Button, &ToolbarBottom);
				static int s_FadeAirBtn = 0;
				if(DoButton_Editor(&s_FadeAirBtn, "Air", m_FadeAirOnUnfilled, &Button, BUTTONFLAG_LEFT, "If on, unfilled cells inside the rect are cleared to air on commit (full replace). If off, unfilled cells are left alone."))
					m_FadeAirOnUnfilled = !m_FadeAirOnUnfilled;
				ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
			}
		}

		// Recent tiles palette (right-aligned)
		if(!m_vRecentTiles.empty())
		{
			const float TileSize = 22.0f;
			const float Spacing = 2.0f;
			const int Count = std::min((int)m_vRecentTiles.size(), 10);
			const float TotalW = Count * (TileSize + Spacing);
			CUIRect PaletteRow;
			ToolbarBottom.VSplitRight(TotalW, &ToolbarBottom, &PaletteRow);
			ToolbarBottom.VSplitRight(5.0f, &ToolbarBottom, nullptr);
			static char s_aPaletteButtonIds[10] = {};
			for(int i = 0; i < Count; ++i)
			{
				CUIRect TileBtn;
				PaletteRow.VSplitLeft(TileSize, &TileBtn, &PaletteRow);
				PaletteRow.VSplitLeft(Spacing, nullptr, &PaletteRow);
				if(DoButton_Editor(&s_aPaletteButtonIds[i], "", 0, &TileBtn, BUTTONFLAG_LEFT, "Recent tile — click to re-select."))
					RecentTilesUse(i);

				const CRecentTile &R = m_vRecentTiles[i];
				IGraphics::CTextureHandle Tex;
				if(R.m_HasGame)
					Tex = GetEntitiesTexture();
				else if(R.m_HasFront)
					Tex = GetFrontTexture();
				else if(R.m_HasTele)
					Tex = GetTeleTexture();
				else if(R.m_HasSwitch)
					Tex = GetSwitchTexture();
				else if(R.m_HasSpeedup)
					Tex = GetSpeedupTexture();
				else if(R.m_HasTune)
					Tex = GetTuneTexture();
				else if(R.m_ImageId >= 0 && R.m_ImageId < (int)Map()->m_vpImages.size())
					Tex = Map()->m_vpImages[R.m_ImageId]->m_Texture;

				if(Tex.IsValid())
				{
					Graphics()->TextureSet(Tex);
					Graphics()->QuadsBegin();
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
					const int Idx = R.m_Tile.m_Index;
					const int Tx = Idx & 0xF;
					const int Ty = (Idx >> 4) & 0xF;
					float U0 = Tx / 16.0f;
					float V0 = Ty / 16.0f;
					float U1 = U0 + 1.0f / 16.0f;
					float V1 = V0 + 1.0f / 16.0f;
					if(R.m_Tile.m_Flags & TILEFLAG_XFLIP)
						std::swap(U0, U1);
					if(R.m_Tile.m_Flags & TILEFLAG_YFLIP)
						std::swap(V0, V1);
					Graphics()->QuadsSetSubset(U0, V0, U1, V1);
					IGraphics::CQuadItem Q(TileBtn.x + 2.0f, TileBtn.y + 2.0f, TileBtn.w - 4.0f, TileBtn.h - 4.0f);
					Graphics()->QuadsDrawTL(&Q, 1);
					Graphics()->QuadsEnd();
				}
			}
		}

		// Symmetry cycle button (Off → H → V → Both → Off)
		{
			ToolbarBottom.VSplitLeft(70.0f, &Button, &ToolbarBottom);
			static int s_SymCycleButton = 0;
			const char *pSymLabel;
			switch(m_SymmetryMode)
			{
			case SYM_H: pSymLabel = "Sym: H"; break;
			case SYM_V: pSymLabel = "Sym: V"; break;
			case SYM_BOTH: pSymLabel = "Sym: Both"; break;
			default: pSymLabel = "Sym: Off"; break;
			}
			if(DoButton_Editor(&s_SymCycleButton, pSymLabel, m_SymmetryMode != SYM_NONE, &Button, BUTTONFLAG_LEFT, "Cycle symmetry: Off → Horizontal → Vertical → Both. Drag axis lines on the canvas to position; in Both mode click the intersection to drag both at once."))
			{
				m_SymmetryMode = (m_SymmetryMode + 1) % 4;
				if(m_SymmetryMode != SYM_NONE && !m_SymmetryCenterInit)
				{
					m_SymmetryCenter = MapView()->GetWorldOffset();
					m_SymmetryCenterInit = true;
				}
			}
			ToolbarBottom.VSplitLeft(5.0f, &Button, &ToolbarBottom);
		}
	}

	// 3D tool floating settings panel — drawn after toolbar so it renders on top
	if(m_ThreeDTool)
	{
		const float PanelW = 215.0f;
		const float PanelPad = 5.0f;
		const float RowH = 16.0f;
		const float SmGap = 2.0f;
		const float MdGap = 4.0f;

		// Height: title(11)+gap(3) + shape(RowH)+gap + render(RowH)+MdGap
		//         + 3×scale(RowH+SmGap)-SmGap+MdGap + 3×rot(RowH+SmGap)-SmGap+MdGap
		//         + persp(RowH)+SmGap + grid(RowH)
		const float ContentH = 14.0f + (RowH + SmGap) + (RowH + MdGap) + (RowH + SmGap) * 3 + MdGap + (RowH + SmGap) * 3 + MdGap + (RowH + SmGap) + RowH;
		const float PanelH = ContentH + PanelPad * 2.0f;

		const CUIRect *pScreen = Ui()->Screen();
		CUIRect Panel = {pScreen->w - PanelW - 4.0f, ToolBar.y + ToolBar.h + 3.0f, PanelW, PanelH};

		Panel.Draw(ColorRGBA(0.08f, 0.08f, 0.08f, 0.93f), IGraphics::CORNER_ALL, 4.0f);
		// border line
		{
			CUIRect Border = {Panel.x, Panel.y, Panel.w, Panel.h};
			Border.Draw(ColorRGBA(0.4f, 0.4f, 0.4f, 0.5f), IGraphics::CORNER_ALL, 4.0f);
		}
		Panel.Margin(PanelPad, &Panel);

		CUIRect Row;

		// Title bar
		Panel.HSplitTop(11.0f, &Row, &Panel);
		Panel.HSplitTop(3.0f, nullptr, &Panel);
		Ui()->DoLabel(&Row, "3D Tool", 9.0f, TEXTALIGN_ML);

		// Shape picker
		Panel.HSplitTop(RowH, &Row, &Panel);
		Panel.HSplitTop(SmGap, nullptr, &Panel);
		{
			static int s_3DCubeBtn = 0, s_3DSphBtn = 0, s_3DCylBtn = 0, s_3DTorusBtn = 0;
			const float Qw = Row.w / 4.0f;
			CUIRect B = Row;
			CUIRect Rest;
			B.VSplitLeft(Qw, &B, &Rest);
			if(DoButton_Ex(&s_3DCubeBtn, "Cube", m_ThreeDShape == THREED_CUBE, &B, BUTTONFLAG_LEFT, "Shape: Cube", IGraphics::CORNER_L))
				m_ThreeDShape = THREED_CUBE;
			Rest.VSplitLeft(Qw, &B, &Rest);
			if(DoButton_Ex(&s_3DSphBtn, "Sph", m_ThreeDShape == THREED_SPHERE, &B, BUTTONFLAG_LEFT, "Shape: Sphere", IGraphics::CORNER_NONE))
				m_ThreeDShape = THREED_SPHERE;
			Rest.VSplitLeft(Qw, &B, &Rest);
			if(DoButton_Ex(&s_3DCylBtn, "Cyl", m_ThreeDShape == THREED_CYLINDER, &B, BUTTONFLAG_LEFT, "Shape: Cylinder", IGraphics::CORNER_NONE))
				m_ThreeDShape = THREED_CYLINDER;
			if(DoButton_Ex(&s_3DTorusBtn, "Torus", m_ThreeDShape == THREED_TORUS, &Rest, BUTTONFLAG_LEFT, "Shape: Torus (ScX=ring r, ScY=tube r, ScZ=Z stretch)", IGraphics::CORNER_R))
				m_ThreeDShape = THREED_TORUS;
		}

		// Render mode
		Panel.HSplitTop(RowH, &Row, &Panel);
		Panel.HSplitTop(MdGap, nullptr, &Panel);
		{
			static int s_3DGhostBtn = 0, s_3DWireBtn = 0, s_3DBothBtn = 0;
			const float Tw = Row.w / 3.0f;
			CUIRect B = Row, Rest;
			B.VSplitLeft(Tw, &B, &Rest);
			if(DoButton_Ex(&s_3DGhostBtn, "Ghost", m_ThreeDRenderMode == THREED_RENDER_GHOST, &B, BUTTONFLAG_LEFT, "Show ghost tile blocks only.", IGraphics::CORNER_L))
				m_ThreeDRenderMode = THREED_RENDER_GHOST;
			Rest.VSplitLeft(Tw, &B, &Rest);
			if(DoButton_Ex(&s_3DWireBtn, "Wire", m_ThreeDRenderMode == THREED_RENDER_WIRE, &B, BUTTONFLAG_LEFT, "Show wireframe lines only.", IGraphics::CORNER_NONE))
				m_ThreeDRenderMode = THREED_RENDER_WIRE;
			if(DoButton_Ex(&s_3DBothBtn, "Both", m_ThreeDRenderMode == THREED_RENDER_BOTH, &Rest, BUTTONFLAG_LEFT, "Ghost blocks + wireframe.", IGraphics::CORNER_R))
				m_ThreeDRenderMode = THREED_RENDER_BOTH;
		}

		// Scale sliders
		{
			static int s_3DScXSel = 0, s_3DScYSel = 0, s_3DScZSel = 0;
			Panel.HSplitTop(RowH, &Row, &Panel);
			Panel.HSplitTop(SmGap, nullptr, &Panel);
			{
				int V = maximum(1, (int)roundf(m_ThreeDScaleX / 32.0f));
				auto Res = UiDoValueSelector(&s_3DScXSel, &Row, "ScX:", V, 1, 1000, 1, 5.0f, "X scale in tiles.");
				m_ThreeDScaleX = (float)Res.m_Value * 32.0f;
			}
			Panel.HSplitTop(RowH, &Row, &Panel);
			Panel.HSplitTop(SmGap, nullptr, &Panel);
			{
				int V = maximum(1, (int)roundf(m_ThreeDScaleY / 32.0f));
				auto Res = UiDoValueSelector(&s_3DScYSel, &Row, "ScY:", V, 1, 1000, 1, 5.0f, "Y scale in tiles.");
				m_ThreeDScaleY = (float)Res.m_Value * 32.0f;
			}
			Panel.HSplitTop(RowH, &Row, &Panel);
			Panel.HSplitTop(MdGap, nullptr, &Panel);
			{
				int V = maximum(1, (int)roundf(m_ThreeDScaleZ / 32.0f));
				auto Res = UiDoValueSelector(&s_3DScZSel, &Row, "ScZ:", V, 1, 1000, 1, 5.0f, "Z scale in tiles.");
				m_ThreeDScaleZ = (float)Res.m_Value * 32.0f;
			}
		}

		// Rotation sliders
		{
			static int s_3DRxSel = 0, s_3DRySel = 0, s_3DRzSel = 0;
			Panel.HSplitTop(RowH, &Row, &Panel);
			Panel.HSplitTop(SmGap, nullptr, &Panel);
			{
				auto Res = UiDoValueSelector(&s_3DRxSel, &Row, "Rx:", (int)m_ThreeDRxDeg, -180, 180, 1, 5.0f, "X rotation in degrees.");
				m_ThreeDRxDeg = (float)Res.m_Value;
			}
			Panel.HSplitTop(RowH, &Row, &Panel);
			Panel.HSplitTop(SmGap, nullptr, &Panel);
			{
				auto Res = UiDoValueSelector(&s_3DRySel, &Row, "Ry:", (int)m_ThreeDRyDeg, -180, 180, 1, 5.0f, "Y rotation in degrees.");
				m_ThreeDRyDeg = (float)Res.m_Value;
			}
			Panel.HSplitTop(RowH, &Row, &Panel);
			Panel.HSplitTop(MdGap, nullptr, &Panel);
			{
				auto Res = UiDoValueSelector(&s_3DRzSel, &Row, "Rz:", (int)m_ThreeDRzDeg, -180, 180, 1, 5.0f, "Z rotation in degrees.");
				m_ThreeDRzDeg = (float)Res.m_Value;
			}
		}

		// Perspective + Grid sliders
		{
			static int s_3DPerspSel = 0, s_3DGridSel = 0;
			Panel.HSplitTop(RowH, &Row, &Panel);
			Panel.HSplitTop(SmGap, nullptr, &Panel);
			{
				auto Res = UiDoValueSelector(&s_3DPerspSel, &Row, "Persp:", (int)m_ThreeDPerspTiles, 1, 200, 1, 5.0f, "Perspective distance in tiles. Larger = more orthographic.");
				m_ThreeDPerspTiles = (float)Res.m_Value;
			}
			Panel.HSplitTop(RowH, &Row, &Panel);
			{
				auto Res = UiDoValueSelector(&s_3DGridSel, &Row, "Grid:", m_ThreeDGridDiv, 1, 16, 1, 5.0f, "Grid subdivisions. 1 = edges only, higher = denser grid.");
				m_ThreeDGridDiv = Res.m_Value;
			}
		}
	}
}

void CEditor::DoToolbarImages(CUIRect ToolBar)
{
	CUIRect ToolBarTop, ToolBarBottom;
	ToolBar.HSplitMid(&ToolBarTop, &ToolBarBottom, 5.0f);

	std::shared_ptr<CEditorImage> pSelectedImage = Map()->SelectedImage();
	if(pSelectedImage != nullptr)
	{
		char aLabel[64];
		str_format(aLabel, sizeof(aLabel), "Size: %" PRIzu " × %" PRIzu, pSelectedImage->m_Width, pSelectedImage->m_Height);
		Ui()->DoLabel(&ToolBarBottom, aLabel, 12.0f, TEXTALIGN_ML);
	}
}

void CEditor::DoToolbarSounds(CUIRect ToolBar)
{
	CUIRect ToolBarTop, ToolBarBottom;
	ToolBar.HSplitMid(&ToolBarTop, &ToolBarBottom, 5.0f);

	std::shared_ptr<CEditorSound> pSelectedSound = Map()->SelectedSound();
	if(pSelectedSound != nullptr)
	{
		if(pSelectedSound->m_SoundId != m_ToolbarPreviewSound && m_ToolbarPreviewSound >= 0 && Sound()->IsPlaying(m_ToolbarPreviewSound))
			Sound()->Stop(m_ToolbarPreviewSound);
		m_ToolbarPreviewSound = pSelectedSound->m_SoundId;
	}
	else
	{
		m_ToolbarPreviewSound = -1;
	}

	if(m_ToolbarPreviewSound >= 0)
	{
		static int s_PlayPauseButton, s_StopButton, s_SeekBar = 0;
		DoAudioPreview(ToolBarBottom, &s_PlayPauseButton, &s_StopButton, &s_SeekBar, m_ToolbarPreviewSound);
	}
}

static void Rotate(const CPoint *pCenter, CPoint *pPoint, float Rotation)
{
	int x = pPoint->x - pCenter->x;
	int y = pPoint->y - pCenter->y;
	pPoint->x = (int)(x * std::cos(Rotation) - y * std::sin(Rotation) + pCenter->x);
	pPoint->y = (int)(x * std::sin(Rotation) + y * std::cos(Rotation) + pCenter->y);
}

void CEditor::DoSoundSource(int LayerIndex, CSoundSource *pSource, int Index)
{
	static ESoundSourceOp s_Operation = ESoundSourceOp::NONE;

	float CenterX = fx2f(pSource->m_Position.x);
	float CenterY = fx2f(pSource->m_Position.y);

	const bool IgnoreGrid = Input()->AltIsPressed();

	if(s_Operation == ESoundSourceOp::NONE)
	{
		if(!Ui()->MouseButton(0))
			Map()->m_SoundSourceOperationTracker.End();
	}

	if(Ui()->CheckActiveItem(pSource))
	{
		if(s_Operation != ESoundSourceOp::NONE)
		{
			Map()->m_SoundSourceOperationTracker.Begin(pSource, s_Operation, LayerIndex);
		}

		if(m_MouseDeltaWorld != vec2(0.0f, 0.0f))
		{
			if(s_Operation == ESoundSourceOp::MOVE)
			{
				vec2 Pos = Ui()->MouseWorldPos();
				if(MapView()->MapGrid()->IsEnabled() && !IgnoreGrid)
				{
					MapView()->MapGrid()->SnapToGrid(Pos);
				}
				pSource->m_Position.x = f2fx(Pos.x);
				pSource->m_Position.y = f2fx(Pos.y);
			}
		}

		if(s_Operation == ESoundSourceOp::CONTEXT_MENU)
		{
			if(!Ui()->MouseButton(1))
			{
				if(Map()->m_vSelectedLayers.size() == 1)
				{
					static SPopupMenuId s_PopupSourceId;
					Ui()->DoPopupMenu(&s_PopupSourceId, Ui()->MouseX(), Ui()->MouseY(), 120, 200, this, PopupSource);
					Ui()->DisableMouseLock();
				}
				s_Operation = ESoundSourceOp::NONE;
				Ui()->SetActiveItem(nullptr);
			}
		}
		else
		{
			if(!Ui()->MouseButton(0))
			{
				Ui()->DisableMouseLock();
				s_Operation = ESoundSourceOp::NONE;
				Ui()->SetActiveItem(nullptr);
			}
		}

		Graphics()->SetColor(1, 1, 1, 1);
	}
	else if(Ui()->HotItem() == pSource)
	{
		m_pUiGotContext = pSource;

		Graphics()->SetColor(1, 1, 1, 1);
		str_copy(m_aTooltip, "Left mouse button to move. Hold alt to ignore grid.");

		if(Ui()->MouseButton(0))
		{
			s_Operation = ESoundSourceOp::MOVE;

			Ui()->SetActiveItem(pSource);
			Map()->m_SelectedSoundSource = Index;
		}

		if(Ui()->MouseButton(1))
		{
			Map()->m_SelectedSoundSource = Index;
			s_Operation = ESoundSourceOp::CONTEXT_MENU;
			Ui()->SetActiveItem(pSource);
		}
	}
	else
	{
		Graphics()->SetColor(0, 1, 0, 1);
	}

	IGraphics::CQuadItem QuadItem(CenterX, CenterY, 5.0f * m_MouseWorldScale, 5.0f * m_MouseWorldScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CEditor::UpdateHotSoundSource(const CLayerSounds *pLayer)
{
	const vec2 MouseWorld = Ui()->MouseWorldPos();

	float MinDist = 500.0f;
	const void *pMinSourceId = nullptr;

	const auto UpdateMinimum = [&](vec2 Position, const void *pId) {
		const float CurrDist = length_squared((Position - MouseWorld) / m_MouseWorldScale);
		if(CurrDist < MinDist)
		{
			MinDist = CurrDist;
			pMinSourceId = pId;
		}
	};

	for(const CSoundSource &Source : pLayer->m_vSources)
	{
		UpdateMinimum(vec2(fx2f(Source.m_Position.x), fx2f(Source.m_Position.y)), &Source);
	}

	if(pMinSourceId != nullptr)
	{
		Ui()->SetHotItem(pMinSourceId);
	}
}

void CEditor::PreparePointDrag(const CQuad *pQuad, int QuadIndex, int PointIndex)
{
	m_QuadDragOriginalPoints[QuadIndex][PointIndex] = pQuad->m_aPoints[PointIndex];
}

void CEditor::DoPointDrag(CQuad *pQuad, int QuadIndex, int PointIndex, ivec2 Offset)
{
	pQuad->m_aPoints[PointIndex] = m_QuadDragOriginalPoints[QuadIndex][PointIndex] + Offset;
}

CEditor::EAxis CEditor::GetDragAxis(ivec2 Offset) const
{
	if(Input()->ShiftIsPressed())
		if(absolute(Offset.x) < absolute(Offset.y))
			return EAxis::Y;
		else
			return EAxis::X;
	else
		return EAxis::NONE;
}

void CEditor::DrawAxis(EAxis Axis, CPoint &OriginalPoint, CPoint &Point) const
{
	if(Axis == EAxis::NONE)
		return;

	Graphics()->SetColor(1, 0, 0.1f, 1);
	if(Axis == EAxis::X)
	{
		IGraphics::CQuadItem Line(fx2f(OriginalPoint.x + Point.x) / 2.0f, fx2f(OriginalPoint.y), fx2f(Point.x - OriginalPoint.x), 1.0f * m_MouseWorldScale);
		Graphics()->QuadsDraw(&Line, 1);
	}
	else if(Axis == EAxis::Y)
	{
		IGraphics::CQuadItem Line(fx2f(OriginalPoint.x), fx2f(OriginalPoint.y + Point.y) / 2.0f, 1.0f * m_MouseWorldScale, fx2f(Point.y - OriginalPoint.y));
		Graphics()->QuadsDraw(&Line, 1);
	}

	// Draw ghost of original point
	IGraphics::CQuadItem QuadItem(fx2f(OriginalPoint.x), fx2f(OriginalPoint.y), 5.0f * m_MouseWorldScale, 5.0f * m_MouseWorldScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CEditor::ComputePointAlignments(const std::shared_ptr<CLayerQuads> &pLayer, CQuad *pQuad, int QuadIndex, int PointIndex, ivec2 Offset, std::vector<SAlignmentInfo> &vAlignments, bool Append) const
{
	if(!Append)
		vAlignments.clear();
	if(!g_Config.m_EdAlignQuads)
		return;

	bool GridEnabled = MapView()->MapGrid()->IsEnabled() && !Input()->AltIsPressed();

	// Perform computation from the original position of this point
	int Threshold = f2fx(maximum(5.0f, 10.0f * m_MouseWorldScale));
	CPoint OrigPoint = m_QuadDragOriginalPoints.at(QuadIndex)[PointIndex];
	// Get the "current" point by applying the offset
	CPoint Point = OrigPoint + Offset;

	// Save smallest diff on both axis to only keep closest alignments
	ivec2 SmallestDiff = ivec2(Threshold + 1, Threshold + 1);
	// Store both axis alignments in separate vectors
	std::vector<SAlignmentInfo> vAlignmentsX, vAlignmentsY;

	// Check if we can align/snap to a specific point
	auto &&CheckAlignment = [&](CPoint *pQuadPoint) {
		ivec2 DirectedDiff = *pQuadPoint - Point;
		ivec2 Diff = ivec2(absolute(DirectedDiff.x), absolute(DirectedDiff.y));

		if(Diff.x <= Threshold && (!GridEnabled || Diff.x == 0))
		{
			// Only store alignments that have the smallest difference
			if(Diff.x < SmallestDiff.x)
			{
				vAlignmentsX.clear();
				SmallestDiff.x = Diff.x;
			}

			// We can have multiple alignments having the same difference/distance
			if(Diff.x == SmallestDiff.x)
			{
				vAlignmentsX.push_back(SAlignmentInfo{
					*pQuadPoint, // Aligned point
					{OrigPoint.y}, // Value that can change (which is not snapped), original position
					EAxis::Y, // The alignment axis
					PointIndex, // The index of the point
					DirectedDiff.x,
				});
			}
		}

		if(Diff.y <= Threshold && (!GridEnabled || Diff.y == 0))
		{
			// Only store alignments that have the smallest difference
			if(Diff.y < SmallestDiff.y)
			{
				vAlignmentsY.clear();
				SmallestDiff.y = Diff.y;
			}

			if(Diff.y == SmallestDiff.y)
			{
				vAlignmentsY.push_back(SAlignmentInfo{
					*pQuadPoint,
					{OrigPoint.x},
					EAxis::X,
					PointIndex,
					DirectedDiff.y,
				});
			}
		}
	};

	// Iterate through all the quads of the current layer
	// Check alignment with each point of the quad (corners & pivot)
	// Compute an AABB (Axis Aligned Bounding Box) to get the center of the quad
	// Check alignment with the center of the quad
	for(size_t i = 0; i < pLayer->m_vQuads.size(); i++)
	{
		auto *pCurrentQuad = &pLayer->m_vQuads[i];
		CPoint Min = pCurrentQuad->m_aPoints[0];
		CPoint Max = pCurrentQuad->m_aPoints[0];

		for(int v = 0; v < 5; v++)
		{
			CPoint *pQuadPoint = &pCurrentQuad->m_aPoints[v];

			if(v != 4)
			{ // Don't use pivot to compute AABB
				if(pQuadPoint->x < Min.x)
					Min.x = pQuadPoint->x;
				if(pQuadPoint->y < Min.y)
					Min.y = pQuadPoint->y;
				if(pQuadPoint->x > Max.x)
					Max.x = pQuadPoint->x;
				if(pQuadPoint->y > Max.y)
					Max.y = pQuadPoint->y;
			}

			// Don't check alignment with current point
			if(pQuadPoint == &pQuad->m_aPoints[PointIndex])
				continue;

			// Don't check alignment with other selected points
			bool IsCurrentPointSelected = Map()->IsQuadSelected(i) && (Map()->IsQuadCornerSelected(v) || (v == PointIndex && PointIndex == 4));
			if(IsCurrentPointSelected)
				continue;

			CheckAlignment(pQuadPoint);
		}

		// Don't check alignment with center of selected quads
		if(!Map()->IsQuadSelected(i))
		{
			CPoint Center = (Min + Max) / 2.0f;
			CheckAlignment(&Center);
		}
	}

	// Finally concatenate both alignment vectors into the output
	vAlignments.reserve(vAlignmentsX.size() + vAlignmentsY.size());
	vAlignments.insert(vAlignments.end(), vAlignmentsX.begin(), vAlignmentsX.end());
	vAlignments.insert(vAlignments.end(), vAlignmentsY.begin(), vAlignmentsY.end());
}

void CEditor::ComputePointsAlignments(const std::shared_ptr<CLayerQuads> &pLayer, bool Pivot, ivec2 Offset, std::vector<SAlignmentInfo> &vAlignments) const
{
	// This method is used to compute alignments from selected points
	// and only apply the closest alignment on X and Y to the offset.

	vAlignments.clear();
	std::vector<SAlignmentInfo> vAllAlignments;

	for(int Selected : Map()->m_vSelectedQuads)
	{
		CQuad *pQuad = &pLayer->m_vQuads[Selected];

		if(!Pivot)
		{
			for(int m = 0; m < 4; m++)
			{
				if(Map()->IsQuadPointSelected(Selected, m))
				{
					ComputePointAlignments(pLayer, pQuad, Selected, m, Offset, vAllAlignments, true);
				}
			}
		}
		else
		{
			ComputePointAlignments(pLayer, pQuad, Selected, 4, Offset, vAllAlignments, true);
		}
	}

	ivec2 SmallestDiff = ivec2(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
	std::vector<SAlignmentInfo> vAlignmentsX, vAlignmentsY;

	for(const auto &Alignment : vAllAlignments)
	{
		int AbsDiff = absolute(Alignment.m_Diff);
		if(Alignment.m_Axis == EAxis::X)
		{
			if(AbsDiff < SmallestDiff.y)
			{
				SmallestDiff.y = AbsDiff;
				vAlignmentsY.clear();
			}
			if(AbsDiff == SmallestDiff.y)
				vAlignmentsY.emplace_back(Alignment);
		}
		else if(Alignment.m_Axis == EAxis::Y)
		{
			if(AbsDiff < SmallestDiff.x)
			{
				SmallestDiff.x = AbsDiff;
				vAlignmentsX.clear();
			}
			if(AbsDiff == SmallestDiff.x)
				vAlignmentsX.emplace_back(Alignment);
		}
	}

	vAlignments.reserve(vAlignmentsX.size() + vAlignmentsY.size());
	vAlignments.insert(vAlignments.end(), vAlignmentsX.begin(), vAlignmentsX.end());
	vAlignments.insert(vAlignments.end(), vAlignmentsY.begin(), vAlignmentsY.end());
}

void CEditor::ComputeAABBAlignments(const std::shared_ptr<CLayerQuads> &pLayer, const SAxisAlignedBoundingBox &AABB, ivec2 Offset, std::vector<SAlignmentInfo> &vAlignments) const
{
	vAlignments.clear();
	if(!g_Config.m_EdAlignQuads)
		return;

	// This method is a bit different than the point alignment in the way where instead of trying to align 1 point to all quads,
	// we try to align 5 points to all quads, these 5 points being 5 points of an AABB.
	// Otherwise, the concept is the same, we use the original position of the AABB to make the computations.
	int Threshold = f2fx(maximum(5.0f, 10.0f * m_MouseWorldScale));
	ivec2 SmallestDiff = ivec2(Threshold + 1, Threshold + 1);
	std::vector<SAlignmentInfo> vAlignmentsX, vAlignmentsY;

	bool GridEnabled = MapView()->MapGrid()->IsEnabled() && !Input()->AltIsPressed();

	auto &&CheckAlignment = [&](CPoint &Aligned, int Point) {
		CPoint ToCheck = AABB.m_aPoints[Point] + Offset;
		ivec2 DirectedDiff = Aligned - ToCheck;
		ivec2 Diff = ivec2(absolute(DirectedDiff.x), absolute(DirectedDiff.y));

		if(Diff.x <= Threshold && (!GridEnabled || Diff.x == 0))
		{
			if(Diff.x < SmallestDiff.x)
			{
				SmallestDiff.x = Diff.x;
				vAlignmentsX.clear();
			}

			if(Diff.x == SmallestDiff.x)
			{
				vAlignmentsX.push_back(SAlignmentInfo{
					Aligned,
					{AABB.m_aPoints[Point].y},
					EAxis::Y,
					Point,
					DirectedDiff.x,
				});
			}
		}

		if(Diff.y <= Threshold && (!GridEnabled || Diff.y == 0))
		{
			if(Diff.y < SmallestDiff.y)
			{
				SmallestDiff.y = Diff.y;
				vAlignmentsY.clear();
			}

			if(Diff.y == SmallestDiff.y)
			{
				vAlignmentsY.push_back(SAlignmentInfo{
					Aligned,
					{AABB.m_aPoints[Point].x},
					EAxis::X,
					Point,
					DirectedDiff.y,
				});
			}
		}
	};

	auto &&CheckAABBAlignment = [&](CPoint &QuadMin, CPoint &QuadMax) {
		CPoint QuadCenter = (QuadMin + QuadMax) / 2.0f;
		CPoint aQuadPoints[5] = {
			QuadMin, // Top left
			{QuadMax.x, QuadMin.y}, // Top right
			{QuadMin.x, QuadMax.y}, // Bottom left
			QuadMax, // Bottom right
			QuadCenter,
		};

		// Check all points with all the other points
		for(auto &QuadPoint : aQuadPoints)
		{
			// i is the quad point which is "aligned" and that we want to compare with
			for(int j = 0; j < 5; j++)
			{
				// j is the point we try to align
				CheckAlignment(QuadPoint, j);
			}
		}
	};

	// Iterate through all quads of the current layer
	// Compute AABB of all quads and check if the dragged AABB can be aligned to this AABB.
	for(size_t i = 0; i < pLayer->m_vQuads.size(); i++)
	{
		auto *pCurrentQuad = &pLayer->m_vQuads[i];
		if(Map()->IsQuadSelected(i)) // Don't check with other selected quads
			continue;

		// Get AABB of this quad
		CPoint QuadMin = pCurrentQuad->m_aPoints[0], QuadMax = pCurrentQuad->m_aPoints[0];
		for(int v = 1; v < 4; v++)
		{
			QuadMin.x = minimum(QuadMin.x, pCurrentQuad->m_aPoints[v].x);
			QuadMin.y = minimum(QuadMin.y, pCurrentQuad->m_aPoints[v].y);
			QuadMax.x = maximum(QuadMax.x, pCurrentQuad->m_aPoints[v].x);
			QuadMax.y = maximum(QuadMax.y, pCurrentQuad->m_aPoints[v].y);
		}

		CheckAABBAlignment(QuadMin, QuadMax);
	}

	// Finally, concatenate both alignment vectors into the output
	vAlignments.reserve(vAlignmentsX.size() + vAlignmentsY.size());
	vAlignments.insert(vAlignments.end(), vAlignmentsX.begin(), vAlignmentsX.end());
	vAlignments.insert(vAlignments.end(), vAlignmentsY.begin(), vAlignmentsY.end());
}

void CEditor::DrawPointAlignments(const std::vector<SAlignmentInfo> &vAlignments, ivec2 Offset) const
{
	if(!g_Config.m_EdAlignQuads)
		return;

	// Drawing an alignment is easy, we convert fixed to float for the aligned point coords
	// and we also convert the "changing" value after applying the offset (which might be edited to actually align the value with the alignment).
	Graphics()->SetColor(1, 0, 0.1f, 1);
	for(const SAlignmentInfo &Alignment : vAlignments)
	{
		// We don't use IGraphics::CLineItem to draw because we don't want to stop QuadsBegin(), quads work just fine.
		if(Alignment.m_Axis == EAxis::X)
		{ // Alignment on X axis is same Y values but different X values
			IGraphics::CQuadItem Line(fx2f(Alignment.m_AlignedPoint.x), fx2f(Alignment.m_AlignedPoint.y), fx2f(Alignment.m_X + Offset.x - Alignment.m_AlignedPoint.x), 1.0f * m_MouseWorldScale);
			Graphics()->QuadsDrawTL(&Line, 1);
		}
		else if(Alignment.m_Axis == EAxis::Y)
		{ // Alignment on Y axis is same X values but different Y values
			IGraphics::CQuadItem Line(fx2f(Alignment.m_AlignedPoint.x), fx2f(Alignment.m_AlignedPoint.y), 1.0f * m_MouseWorldScale, fx2f(Alignment.m_Y + Offset.y - Alignment.m_AlignedPoint.y));
			Graphics()->QuadsDrawTL(&Line, 1);
		}
	}
}

void CEditor::DrawAABB(const SAxisAlignedBoundingBox &AABB, ivec2 Offset) const
{
	// Drawing an AABB is simply converting the points from fixed to float
	// Then making lines out of quads and drawing them
	vec2 TL = {fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_TL].x + Offset.x), fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_TL].y + Offset.y)};
	vec2 TR = {fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_TR].x + Offset.x), fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_TR].y + Offset.y)};
	vec2 BL = {fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_BL].x + Offset.x), fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_BL].y + Offset.y)};
	vec2 BR = {fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_BR].x + Offset.x), fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_BR].y + Offset.y)};
	vec2 Center = {fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_CENTER].x + Offset.x), fx2f(AABB.m_aPoints[SAxisAlignedBoundingBox::POINT_CENTER].y + Offset.y)};

	// We don't use IGraphics::CLineItem to draw because we don't want to stop QuadsBegin(), quads work just fine.
	IGraphics::CQuadItem Lines[4] = {
		{TL.x, TL.y, TR.x - TL.x, 1.0f * m_MouseWorldScale},
		{TL.x, TL.y, 1.0f * m_MouseWorldScale, BL.y - TL.y},
		{TR.x, TR.y, 1.0f * m_MouseWorldScale, BR.y - TR.y},
		{BL.x, BL.y, BR.x - BL.x, 1.0f * m_MouseWorldScale},
	};
	Graphics()->SetColor(1, 0, 1, 1);
	Graphics()->QuadsDrawTL(Lines, 4);

	IGraphics::CQuadItem CenterQuad(Center.x, Center.y, 5.0f * m_MouseWorldScale, 5.0f * m_MouseWorldScale);
	Graphics()->QuadsDraw(&CenterQuad, 1);
}

void CEditor::QuadSelectionAABB(const std::shared_ptr<CLayerQuads> &pLayer, SAxisAlignedBoundingBox &OutAABB)
{
	// Compute an englobing AABB of the current selection of quads
	CPoint Min{
		std::numeric_limits<int>::max(),
		std::numeric_limits<int>::max(),
	};
	CPoint Max{
		std::numeric_limits<int>::min(),
		std::numeric_limits<int>::min(),
	};
	for(int Selected : Map()->m_vSelectedQuads)
	{
		CQuad *pQuad = &pLayer->m_vQuads[Selected];
		for(int i = 0; i < 4; i++)
		{
			auto *pPoint = &pQuad->m_aPoints[i];
			Min.x = minimum(Min.x, pPoint->x);
			Min.y = minimum(Min.y, pPoint->y);
			Max.x = maximum(Max.x, pPoint->x);
			Max.y = maximum(Max.y, pPoint->y);
		}
	}
	CPoint Center = (Min + Max) / 2.0f;
	CPoint aPoints[SAxisAlignedBoundingBox::NUM_POINTS] = {
		Min, // Top left
		{Max.x, Min.y}, // Top right
		{Min.x, Max.y}, // Bottom left
		Max, // Bottom right
		Center,
	};
	mem_copy(OutAABB.m_aPoints, aPoints, sizeof(CPoint) * SAxisAlignedBoundingBox::NUM_POINTS);
}

void CEditor::ApplyAlignments(const std::vector<SAlignmentInfo> &vAlignments, ivec2 &Offset)
{
	if(vAlignments.empty())
		return;

	// To find the alignments we simply iterate through the vector of alignments and find the first
	// X and Y alignments.
	// Then, we use the saved m_Diff to adjust the offset
	bvec2 GotAdjust = bvec2(false, false);
	ivec2 Adjust = ivec2(0, 0);
	for(const SAlignmentInfo &Alignment : vAlignments)
	{
		if(Alignment.m_Axis == EAxis::X && !GotAdjust.y)
		{
			GotAdjust.y = true;
			Adjust.y = Alignment.m_Diff;
		}
		else if(Alignment.m_Axis == EAxis::Y && !GotAdjust.x)
		{
			GotAdjust.x = true;
			Adjust.x = Alignment.m_Diff;
		}
	}

	Offset += Adjust;
}

void CEditor::ApplyAxisAlignment(ivec2 &Offset) const
{
	// This is used to preserve axis alignment when pressing `Shift`
	// Should be called before any other computation
	EAxis Axis = GetDragAxis(Offset);
	Offset.x = ((Axis == EAxis::NONE || Axis == EAxis::X) ? Offset.x : 0);
	Offset.y = ((Axis == EAxis::NONE || Axis == EAxis::Y) ? Offset.y : 0);
}

static CColor AverageColor(const std::vector<CQuad *> &vpQuads)
{
	CColor Average = {0, 0, 0, 0};
	for(CQuad *pQuad : vpQuads)
	{
		for(CColor Color : pQuad->m_aColors)
		{
			Average += Color;
		}
	}
	return Average / std::size(CQuad{}.m_aColors) / vpQuads.size();
}

void CEditor::DoQuad(int LayerIndex, const std::shared_ptr<CLayerQuads> &pLayer, CQuad *pQuad, int Index)
{
	enum
	{
		OP_NONE = 0,
		OP_SELECT,
		OP_MOVE_ALL,
		OP_MOVE_PIVOT,
		OP_ROTATE,
		OP_CONTEXT_MENU,
		OP_DELETE,
	};

	// some basic values
	const void *pId = &pQuad->m_aPoints[4]; // use pivot addr as id
	static std::vector<std::vector<CPoint>> s_vvRotatePoints;
	static int s_Operation = OP_NONE;
	static vec2 s_MouseStart = vec2(0.0f, 0.0f);
	static float s_RotateAngle = 0;
	static CPoint s_OriginalPosition;
	static std::vector<SAlignmentInfo> s_PivotAlignments; // Alignments per pivot per quad
	static std::vector<SAlignmentInfo> s_vAABBAlignments; // Alignments for one AABB (single quad or selection of multiple quads)
	static SAxisAlignedBoundingBox s_SelectionAABB; // Selection AABB
	static ivec2 s_LastOffset; // Last offset, stored as static so we can use it to draw every frame

	// get pivot
	float CenterX = fx2f(pQuad->m_aPoints[4].x);
	float CenterY = fx2f(pQuad->m_aPoints[4].y);

	const bool IgnoreGrid = Input()->AltIsPressed();

	auto &&GetDragOffset = [&]() -> ivec2 {
		vec2 Pos = Ui()->MouseWorldPos();
		if(MapView()->MapGrid()->IsEnabled() && !IgnoreGrid)
		{
			MapView()->MapGrid()->SnapToGrid(Pos);
		}
		return ivec2(f2fx(Pos.x) - s_OriginalPosition.x, f2fx(Pos.y) - s_OriginalPosition.y);
	};

	// draw selection background
	if(Map()->IsQuadSelected(Index))
	{
		Graphics()->SetColor(0, 0, 0, 1);
		IGraphics::CQuadItem QuadItem(CenterX, CenterY, 7.0f * m_MouseWorldScale, 7.0f * m_MouseWorldScale);
		Graphics()->QuadsDraw(&QuadItem, 1);
	}

	if(Ui()->CheckActiveItem(pId))
	{
		if(m_MouseDeltaWorld != vec2(0.0f, 0.0f))
		{
			if(s_Operation == OP_SELECT)
			{
				if(length_squared(s_MouseStart - Ui()->MousePos()) > 20.0f)
				{
					if(!Map()->IsQuadSelected(Index))
						Map()->SelectQuad(Index);

					s_OriginalPosition = pQuad->m_aPoints[4];

					if(Input()->ShiftIsPressed())
					{
						s_Operation = OP_MOVE_PIVOT;
						// When moving, we need to save the original position of all selected pivots
						for(int Selected : Map()->m_vSelectedQuads)
						{
							const CQuad *pCurrentQuad = &pLayer->m_vQuads[Selected];
							PreparePointDrag(pCurrentQuad, Selected, 4);
						}
					}
					else
					{
						s_Operation = OP_MOVE_ALL;
						// When moving, we need to save the original position of all selected quads points
						for(int Selected : Map()->m_vSelectedQuads)
						{
							const CQuad *pCurrentQuad = &pLayer->m_vQuads[Selected];
							for(size_t v = 0; v < 5; v++)
								PreparePointDrag(pCurrentQuad, Selected, v);
						}
						// And precompute AABB of selection since it will not change during drag
						if(g_Config.m_EdAlignQuads)
							QuadSelectionAABB(pLayer, s_SelectionAABB);
					}
				}
			}

			// check if we only should move pivot
			if(s_Operation == OP_MOVE_PIVOT)
			{
				Map()->m_QuadTracker.BeginQuadTrack(pLayer, Map()->m_vSelectedQuads, -1, LayerIndex);

				s_LastOffset = GetDragOffset(); // Update offset
				ApplyAxisAlignment(s_LastOffset); // Apply axis alignment to the offset

				ComputePointsAlignments(pLayer, true, s_LastOffset, s_PivotAlignments);
				ApplyAlignments(s_PivotAlignments, s_LastOffset);

				for(auto &Selected : Map()->m_vSelectedQuads)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[Selected];
					DoPointDrag(pCurrentQuad, Selected, 4, s_LastOffset);
				}
			}
			else if(s_Operation == OP_MOVE_ALL)
			{
				Map()->m_QuadTracker.BeginQuadTrack(pLayer, Map()->m_vSelectedQuads, -1, LayerIndex);

				// Compute drag offset
				s_LastOffset = GetDragOffset();
				ApplyAxisAlignment(s_LastOffset);

				// Then compute possible alignments with the selection AABB
				ComputeAABBAlignments(pLayer, s_SelectionAABB, s_LastOffset, s_vAABBAlignments);
				// Apply alignments before drag
				ApplyAlignments(s_vAABBAlignments, s_LastOffset);
				// Then do the drag
				for(int Selected : Map()->m_vSelectedQuads)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[Selected];
					for(int v = 0; v < 5; v++)
						DoPointDrag(pCurrentQuad, Selected, v, s_LastOffset);
				}
			}
			else if(s_Operation == OP_ROTATE)
			{
				Map()->m_QuadTracker.BeginQuadTrack(pLayer, Map()->m_vSelectedQuads, -1, LayerIndex);

				for(size_t i = 0; i < Map()->m_vSelectedQuads.size(); ++i)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[Map()->m_vSelectedQuads[i]];
					for(int v = 0; v < 4; v++)
					{
						pCurrentQuad->m_aPoints[v] = s_vvRotatePoints[i][v];
						Rotate(&pCurrentQuad->m_aPoints[4], &pCurrentQuad->m_aPoints[v], s_RotateAngle);
					}
				}

				s_RotateAngle += Ui()->MouseDeltaX() * (Input()->ShiftIsPressed() ? 0.0001f : 0.002f);
			}
		}

		// Draw axis and alignments when moving
		if(s_Operation == OP_MOVE_PIVOT || s_Operation == OP_MOVE_ALL)
		{
			EAxis Axis = GetDragAxis(s_LastOffset);
			DrawAxis(Axis, s_OriginalPosition, pQuad->m_aPoints[4]);

			str_copy(m_aTooltip, "Hold shift to keep alignment on one axis.");
		}

		if(s_Operation == OP_MOVE_PIVOT)
			DrawPointAlignments(s_PivotAlignments, s_LastOffset);

		if(s_Operation == OP_MOVE_ALL)
		{
			DrawPointAlignments(s_vAABBAlignments, s_LastOffset);

			if(g_Config.m_EdShowQuadsRect)
				DrawAABB(s_SelectionAABB, s_LastOffset);
		}

		if(s_Operation == OP_CONTEXT_MENU)
		{
			if(!Ui()->MouseButton(1))
			{
				if(Map()->m_vSelectedLayers.size() == 1)
				{
					m_QuadPopupContext.m_pEditor = this;
					m_QuadPopupContext.m_SelectedQuadIndex = Map()->FindSelectedQuadIndex(Index);
					dbg_assert(m_QuadPopupContext.m_SelectedQuadIndex >= 0, "Selected quad index not found for quad popup");
					m_QuadPopupContext.m_Color = PackColor(AverageColor(Map()->SelectedQuads()));
					Ui()->DoPopupMenu(&m_QuadPopupContext, Ui()->MouseX(), Ui()->MouseY(), 120, 251, &m_QuadPopupContext, PopupQuad);
					Ui()->DisableMouseLock();
				}
				s_Operation = OP_NONE;
				Ui()->SetActiveItem(nullptr);
			}
		}
		else if(s_Operation == OP_DELETE)
		{
			if(!Ui()->MouseButton(1))
			{
				if(Map()->m_vSelectedLayers.size() == 1)
				{
					Ui()->DisableMouseLock();
					Map()->OnModify();
					Map()->DeleteSelectedQuads();
				}
				s_Operation = OP_NONE;
				Ui()->SetActiveItem(nullptr);
			}
		}
		else if(s_Operation == OP_ROTATE)
		{
			if(Ui()->MouseButton(0))
			{
				Ui()->DisableMouseLock();
				s_Operation = OP_NONE;
				Ui()->SetActiveItem(nullptr);
				Map()->m_QuadTracker.EndQuadTrack();
			}
			else if(Ui()->MouseButton(1))
			{
				Ui()->DisableMouseLock();
				s_Operation = OP_NONE;
				Ui()->SetActiveItem(nullptr);

				// Reset points to old position
				for(size_t i = 0; i < Map()->m_vSelectedQuads.size(); ++i)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[Map()->m_vSelectedQuads[i]];
					for(int v = 0; v < 4; v++)
						pCurrentQuad->m_aPoints[v] = s_vvRotatePoints[i][v];
				}
			}
		}
		else
		{
			if(!Ui()->MouseButton(0))
			{
				if(s_Operation == OP_SELECT)
				{
					if(Input()->ShiftIsPressed())
						Map()->ToggleSelectQuad(Index);
					else
						Map()->SelectQuad(Index);
				}
				else if(s_Operation == OP_MOVE_PIVOT || s_Operation == OP_MOVE_ALL)
				{
					Map()->m_QuadTracker.EndQuadTrack();
				}

				Ui()->DisableMouseLock();
				s_Operation = OP_NONE;
				Ui()->SetActiveItem(nullptr);

				s_LastOffset = ivec2();
				s_OriginalPosition = ivec2();
				s_vAABBAlignments.clear();
				s_PivotAlignments.clear();
			}
		}

		Graphics()->SetColor(1, 1, 1, 1);
	}
	else if(Input()->KeyPress(KEY_R) && !Map()->m_vSelectedQuads.empty() && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && !Ui()->IsPopupOpen())
	{
		Ui()->EnableMouseLock(pId);
		Ui()->SetActiveItem(pId);
		s_Operation = OP_ROTATE;
		s_RotateAngle = 0;

		s_vvRotatePoints.clear();
		s_vvRotatePoints.resize(Map()->m_vSelectedQuads.size());
		for(size_t i = 0; i < Map()->m_vSelectedQuads.size(); ++i)
		{
			CQuad *pCurrentQuad = &pLayer->m_vQuads[Map()->m_vSelectedQuads[i]];

			s_vvRotatePoints[i].resize(4);
			s_vvRotatePoints[i][0] = pCurrentQuad->m_aPoints[0];
			s_vvRotatePoints[i][1] = pCurrentQuad->m_aPoints[1];
			s_vvRotatePoints[i][2] = pCurrentQuad->m_aPoints[2];
			s_vvRotatePoints[i][3] = pCurrentQuad->m_aPoints[3];
		}
	}
	else if(Ui()->HotItem() == pId)
	{
		m_pUiGotContext = pId;

		Graphics()->SetColor(1, 1, 1, 1);
		str_copy(m_aTooltip, "Left mouse button to move. Hold shift to move pivot. Hold alt to ignore grid. Shift+right click to delete.");

		if(Ui()->MouseButton(0))
		{
			Ui()->SetActiveItem(pId);

			s_MouseStart = Ui()->MousePos();
			s_Operation = OP_SELECT;
		}
		else if(Ui()->MouseButtonClicked(1))
		{
			if(Input()->ShiftIsPressed())
			{
				s_Operation = OP_DELETE;

				if(!Map()->IsQuadSelected(Index))
					Map()->SelectQuad(Index);

				Ui()->SetActiveItem(pId);
			}
			else
			{
				s_Operation = OP_CONTEXT_MENU;

				if(!Map()->IsQuadSelected(Index))
					Map()->SelectQuad(Index);

				Ui()->SetActiveItem(pId);
			}
		}
	}
	else
		Graphics()->SetColor(0, 1, 0, 1);

	IGraphics::CQuadItem QuadItem(CenterX, CenterY, 5.0f * m_MouseWorldScale, 5.0f * m_MouseWorldScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CEditor::DoQuadPoint(int LayerIndex, const std::shared_ptr<CLayerQuads> &pLayer, CQuad *pQuad, int QuadIndex, int V)
{
	const void *pId = &pQuad->m_aPoints[V];
	const vec2 Center = vec2(fx2f(pQuad->m_aPoints[V].x), fx2f(pQuad->m_aPoints[V].y));
	const bool IgnoreGrid = Input()->AltIsPressed();

	// draw selection background
	if(Map()->IsQuadPointSelected(QuadIndex, V))
	{
		Graphics()->SetColor(0, 0, 0, 1);
		IGraphics::CQuadItem QuadItem(Center.x, Center.y, 7.0f * m_MouseWorldScale, 7.0f * m_MouseWorldScale);
		Graphics()->QuadsDraw(&QuadItem, 1);
	}

	enum
	{
		OP_NONE = 0,
		OP_SELECT,
		OP_MOVEPOINT,
		OP_MOVEUV,
		OP_CONTEXT_MENU
	};

	static int s_Operation = OP_NONE;
	static vec2 s_MouseStart = vec2(0.0f, 0.0f);
	static CPoint s_OriginalPoint;
	static std::vector<SAlignmentInfo> s_Alignments; // Alignments
	static ivec2 s_LastOffset;

	auto &&GetDragOffset = [&]() -> ivec2 {
		vec2 Pos = Ui()->MouseWorldPos();
		if(MapView()->MapGrid()->IsEnabled() && !IgnoreGrid)
		{
			MapView()->MapGrid()->SnapToGrid(Pos);
		}
		return ivec2(f2fx(Pos.x) - s_OriginalPoint.x, f2fx(Pos.y) - s_OriginalPoint.y);
	};

	if(Ui()->CheckActiveItem(pId))
	{
		if(m_MouseDeltaWorld != vec2(0.0f, 0.0f))
		{
			if(s_Operation == OP_SELECT)
			{
				if(length_squared(s_MouseStart - Ui()->MousePos()) > 20.0f)
				{
					if(!Map()->IsQuadPointSelected(QuadIndex, V))
						Map()->SelectQuadPoint(QuadIndex, V);

					if(Input()->ShiftIsPressed())
					{
						s_Operation = OP_MOVEUV;
						Ui()->EnableMouseLock(pId);
					}
					else
					{
						s_Operation = OP_MOVEPOINT;
						// Save original positions before moving
						s_OriginalPoint = pQuad->m_aPoints[V];
						for(int Selected : Map()->m_vSelectedQuads)
						{
							for(int m = 0; m < 4; m++)
								if(Map()->IsQuadPointSelected(Selected, m))
									PreparePointDrag(&pLayer->m_vQuads[Selected], Selected, m);
						}
					}
				}
			}

			if(s_Operation == OP_MOVEPOINT)
			{
				Map()->m_QuadTracker.BeginQuadTrack(pLayer, Map()->m_vSelectedQuads, -1, LayerIndex);

				s_LastOffset = GetDragOffset(); // Update offset
				ApplyAxisAlignment(s_LastOffset); // Apply axis alignment to offset

				ComputePointsAlignments(pLayer, false, s_LastOffset, s_Alignments);
				ApplyAlignments(s_Alignments, s_LastOffset);

				for(int Selected : Map()->m_vSelectedQuads)
				{
					for(int m = 0; m < 4; m++)
					{
						if(Map()->IsQuadPointSelected(Selected, m))
						{
							DoPointDrag(&pLayer->m_vQuads[Selected], Selected, m, s_LastOffset);
						}
					}
				}
			}
			else if(s_Operation == OP_MOVEUV)
			{
				int SelectedPoints = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);

				Map()->m_QuadTracker.BeginQuadPointPropTrack(pLayer, Map()->m_vSelectedQuads, SelectedPoints, -1, LayerIndex);
				Map()->m_QuadTracker.AddQuadPointPropTrack(EQuadPointProp::TEX_U);
				Map()->m_QuadTracker.AddQuadPointPropTrack(EQuadPointProp::TEX_V);

				for(int Selected : Map()->m_vSelectedQuads)
				{
					CQuad *pSelectedQuad = &pLayer->m_vQuads[Selected];
					for(int m = 0; m < 4; m++)
					{
						if(Map()->IsQuadPointSelected(Selected, m))
						{
							// 0,2;1,3 - line x
							// 0,1;2,3 - line y

							pSelectedQuad->m_aTexcoords[m].x += f2fx(m_MouseDeltaWorld.x * 0.001f);
							pSelectedQuad->m_aTexcoords[(m + 2) % 4].x += f2fx(m_MouseDeltaWorld.x * 0.001f);

							pSelectedQuad->m_aTexcoords[m].y += f2fx(m_MouseDeltaWorld.y * 0.001f);
							pSelectedQuad->m_aTexcoords[m ^ 1].y += f2fx(m_MouseDeltaWorld.y * 0.001f);
						}
					}
				}
			}
		}

		// Draw axis and alignments when dragging
		if(s_Operation == OP_MOVEPOINT)
		{
			Graphics()->SetColor(1, 0, 0.1f, 1);

			// Axis
			EAxis Axis = GetDragAxis(s_LastOffset);
			DrawAxis(Axis, s_OriginalPoint, pQuad->m_aPoints[V]);

			// Alignments
			DrawPointAlignments(s_Alignments, s_LastOffset);

			str_copy(m_aTooltip, "Hold shift to keep alignment on one axis.");
		}

		if(s_Operation == OP_CONTEXT_MENU)
		{
			if(!Ui()->MouseButton(1))
			{
				if(Map()->m_vSelectedLayers.size() == 1)
				{
					if(!Map()->IsQuadSelected(QuadIndex))
						Map()->SelectQuad(QuadIndex);

					m_PointPopupContext.m_pEditor = this;
					m_PointPopupContext.m_SelectedQuadPoint = V;
					m_PointPopupContext.m_SelectedQuadIndex = Map()->FindSelectedQuadIndex(QuadIndex);
					dbg_assert(m_PointPopupContext.m_SelectedQuadIndex >= 0, "Selected quad index not found for quad point popup");
					Ui()->DoPopupMenu(&m_PointPopupContext, Ui()->MouseX(), Ui()->MouseY(), 120, 75, &m_PointPopupContext, PopupPoint);
				}
				Ui()->SetActiveItem(nullptr);
			}
		}
		else
		{
			if(!Ui()->MouseButton(0))
			{
				if(s_Operation == OP_SELECT)
				{
					if(Input()->ShiftIsPressed())
						Map()->ToggleSelectQuadPoint(QuadIndex, V);
					else
						Map()->SelectQuadPoint(QuadIndex, V);
				}

				if(s_Operation == OP_MOVEPOINT)
				{
					Map()->m_QuadTracker.EndQuadTrack();
				}
				else if(s_Operation == OP_MOVEUV)
				{
					Map()->m_QuadTracker.EndQuadPointPropTrackAll();
				}

				Ui()->DisableMouseLock();
				Ui()->SetActiveItem(nullptr);
			}
		}

		Graphics()->SetColor(1, 1, 1, 1);
	}
	else if(Ui()->HotItem() == pId)
	{
		m_pUiGotContext = pId;

		Graphics()->SetColor(1, 1, 1, 1);
		str_copy(m_aTooltip, "Left mouse button to move. Hold shift to move the texture. Hold alt to ignore grid.");

		if(Ui()->MouseButton(0))
		{
			Ui()->SetActiveItem(pId);

			s_MouseStart = Ui()->MousePos();
			s_Operation = OP_SELECT;
		}
		else if(Ui()->MouseButtonClicked(1))
		{
			s_Operation = OP_CONTEXT_MENU;

			Ui()->SetActiveItem(pId);

			if(!Map()->IsQuadPointSelected(QuadIndex, V))
				Map()->SelectQuadPoint(QuadIndex, V);
		}
	}
	else
		Graphics()->SetColor(1, 0, 0, 1);

	IGraphics::CQuadItem QuadItem(Center.x, Center.y, 5.0f * m_MouseWorldScale, 5.0f * m_MouseWorldScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

float CEditor::TriangleArea(vec2 A, vec2 B, vec2 C)
{
	return absolute(((B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y)) * 0.5f);
}

bool CEditor::IsInTriangle(vec2 Point, vec2 A, vec2 B, vec2 C)
{
	// Normalize to increase precision
	vec2 Min(minimum(A.x, B.x, C.x), minimum(A.y, B.y, C.y));
	vec2 Max(maximum(A.x, B.x, C.x), maximum(A.y, B.y, C.y));
	vec2 Size(Max.x - Min.x, Max.y - Min.y);

	if(Size.x < 0.0000001f || Size.y < 0.0000001f)
		return false;

	vec2 Normal(1.f / Size.x, 1.f / Size.y);

	A = (A - Min) * Normal;
	B = (B - Min) * Normal;
	C = (C - Min) * Normal;
	Point = (Point - Min) * Normal;

	float Area = TriangleArea(A, B, C);
	return Area > 0.f && absolute(TriangleArea(Point, A, B) + TriangleArea(Point, B, C) + TriangleArea(Point, C, A) - Area) < 0.000001f;
}

void CEditor::DoQuadKnife(int QuadIndex)
{
	if(m_Dialog != DIALOG_NONE || Ui()->IsPopupOpen())
	{
		return;
	}

	std::shared_ptr<CLayerQuads> pLayer = std::static_pointer_cast<CLayerQuads>(Map()->SelectedLayerType(0, LAYERTYPE_QUADS));
	CQuad *pQuad = &pLayer->m_vQuads[QuadIndex];
	CEditorMap::CQuadKnife &QuadKnife = Map()->m_QuadKnife;

	const bool IgnoreGrid = Input()->AltIsPressed();
	float SnapRadius = 4.f * m_MouseWorldScale;

	vec2 Mouse = vec2(Ui()->MouseWorldX(), Ui()->MouseWorldY());
	vec2 Point = Mouse;

	vec2 v[4] = {
		vec2(fx2f(pQuad->m_aPoints[0].x), fx2f(pQuad->m_aPoints[0].y)),
		vec2(fx2f(pQuad->m_aPoints[1].x), fx2f(pQuad->m_aPoints[1].y)),
		vec2(fx2f(pQuad->m_aPoints[3].x), fx2f(pQuad->m_aPoints[3].y)),
		vec2(fx2f(pQuad->m_aPoints[2].x), fx2f(pQuad->m_aPoints[2].y))};

	str_copy(m_aTooltip, "Left click inside the quad to select an area to slice. Hold alt to ignore grid. Right click to leave knife mode.");

	if(Ui()->MouseButtonClicked(1))
	{
		QuadKnife.m_Active = false;
		return;
	}

	// Handle snapping
	if(MapView()->MapGrid()->IsEnabled() && !IgnoreGrid)
	{
		float CellSize = MapView()->MapGrid()->GridLineDistance();
		vec2 OnGrid = Mouse;
		MapView()->MapGrid()->SnapToGrid(OnGrid);

		if(IsInTriangle(OnGrid, v[0], v[1], v[2]) || IsInTriangle(OnGrid, v[0], v[3], v[2]))
			Point = OnGrid;
		else
		{
			float MinDistance = -1.f;

			for(int i = 0; i < 4; i++)
			{
				int j = (i + 1) % 4;
				vec2 Min(minimum(v[i].x, v[j].x), minimum(v[i].y, v[j].y));
				vec2 Max(maximum(v[i].x, v[j].x), maximum(v[i].y, v[j].y));

				if(in_range(OnGrid.y, Min.y, Max.y) && Max.y - Min.y > 0.0000001f)
				{
					vec2 OnEdge(v[i].x + (OnGrid.y - v[i].y) / (v[j].y - v[i].y) * (v[j].x - v[i].x), OnGrid.y);
					float Distance = absolute(OnGrid.x - OnEdge.x);

					if(Distance < CellSize && (Distance < MinDistance || MinDistance < 0.f))
					{
						MinDistance = Distance;
						Point = OnEdge;
					}
				}

				if(in_range(OnGrid.x, Min.x, Max.x) && Max.x - Min.x > 0.0000001f)
				{
					vec2 OnEdge(OnGrid.x, v[i].y + (OnGrid.x - v[i].x) / (v[j].x - v[i].x) * (v[j].y - v[i].y));
					float Distance = absolute(OnGrid.y - OnEdge.y);

					if(Distance < CellSize && (Distance < MinDistance || MinDistance < 0.f))
					{
						MinDistance = Distance;
						Point = OnEdge;
					}
				}
			}
		}
	}
	else
	{
		float MinDistance = -1.f;

		// Try snapping to corners
		for(const auto &x : v)
		{
			float Distance = distance(Mouse, x);

			if(Distance <= SnapRadius && (Distance < MinDistance || MinDistance < 0.f))
			{
				MinDistance = Distance;
				Point = x;
			}
		}

		if(MinDistance < 0.f)
		{
			// Try snapping to edges
			for(int i = 0; i < 4; i++)
			{
				int j = (i + 1) % 4;
				vec2 s(v[j] - v[i]);

				float t = ((Mouse.x - v[i].x) * s.x + (Mouse.y - v[i].y) * s.y) / (s.x * s.x + s.y * s.y);

				if(in_range(t, 0.f, 1.f))
				{
					vec2 OnEdge = vec2((v[i].x + t * s.x), (v[i].y + t * s.y));
					float Distance = distance(Mouse, OnEdge);

					if(Distance <= SnapRadius && (Distance < MinDistance || MinDistance < 0.f))
					{
						MinDistance = Distance;
						Point = OnEdge;
					}
				}
			}
		}
	}

	bool ValidPosition = IsInTriangle(Point, v[0], v[1], v[2]) || IsInTriangle(Point, v[0], v[3], v[2]);

	if(Ui()->MouseButtonClicked(0) && ValidPosition)
	{
		QuadKnife.m_aPoints[QuadKnife.m_Count] = Point;
		QuadKnife.m_Count++;
	}

	if(QuadKnife.m_Count == 4)
	{
		if(IsInTriangle(QuadKnife.m_aPoints[3], QuadKnife.m_aPoints[0], QuadKnife.m_aPoints[1], QuadKnife.m_aPoints[2]) ||
			IsInTriangle(QuadKnife.m_aPoints[1], QuadKnife.m_aPoints[0], QuadKnife.m_aPoints[2], QuadKnife.m_aPoints[3]))
		{
			// Fix concave order
			std::swap(QuadKnife.m_aPoints[0], QuadKnife.m_aPoints[3]);
			std::swap(QuadKnife.m_aPoints[1], QuadKnife.m_aPoints[2]);
		}

		std::swap(QuadKnife.m_aPoints[2], QuadKnife.m_aPoints[3]);

		CQuad *pResult = pLayer->NewQuad(64, 64, 64, 64);
		pQuad = &pLayer->m_vQuads[QuadIndex];

		for(int i = 0; i < 4; i++)
		{
			int t = IsInTriangle(QuadKnife.m_aPoints[i], v[0], v[3], v[2]) ? 2 : 1;

			vec2 A = vec2(fx2f(pQuad->m_aPoints[0].x), fx2f(pQuad->m_aPoints[0].y));
			vec2 B = vec2(fx2f(pQuad->m_aPoints[3].x), fx2f(pQuad->m_aPoints[3].y));
			vec2 C = vec2(fx2f(pQuad->m_aPoints[t].x), fx2f(pQuad->m_aPoints[t].y));

			float TriArea = TriangleArea(A, B, C);
			float WeightA = TriangleArea(QuadKnife.m_aPoints[i], B, C) / TriArea;
			float WeightB = TriangleArea(QuadKnife.m_aPoints[i], C, A) / TriArea;
			float WeightC = TriangleArea(QuadKnife.m_aPoints[i], A, B) / TriArea;

			pResult->m_aColors[i].r = (int)std::round(pQuad->m_aColors[0].r * WeightA + pQuad->m_aColors[3].r * WeightB + pQuad->m_aColors[t].r * WeightC);
			pResult->m_aColors[i].g = (int)std::round(pQuad->m_aColors[0].g * WeightA + pQuad->m_aColors[3].g * WeightB + pQuad->m_aColors[t].g * WeightC);
			pResult->m_aColors[i].b = (int)std::round(pQuad->m_aColors[0].b * WeightA + pQuad->m_aColors[3].b * WeightB + pQuad->m_aColors[t].b * WeightC);
			pResult->m_aColors[i].a = (int)std::round(pQuad->m_aColors[0].a * WeightA + pQuad->m_aColors[3].a * WeightB + pQuad->m_aColors[t].a * WeightC);

			pResult->m_aTexcoords[i].x = (int)std::round(pQuad->m_aTexcoords[0].x * WeightA + pQuad->m_aTexcoords[3].x * WeightB + pQuad->m_aTexcoords[t].x * WeightC);
			pResult->m_aTexcoords[i].y = (int)std::round(pQuad->m_aTexcoords[0].y * WeightA + pQuad->m_aTexcoords[3].y * WeightB + pQuad->m_aTexcoords[t].y * WeightC);

			pResult->m_aPoints[i].x = f2fx(QuadKnife.m_aPoints[i].x);
			pResult->m_aPoints[i].y = f2fx(QuadKnife.m_aPoints[i].y);
		}

		pResult->m_aPoints[4].x = ((pResult->m_aPoints[0].x + pResult->m_aPoints[3].x) / 2 + (pResult->m_aPoints[1].x + pResult->m_aPoints[2].x) / 2) / 2;
		pResult->m_aPoints[4].y = ((pResult->m_aPoints[0].y + pResult->m_aPoints[3].y) / 2 + (pResult->m_aPoints[1].y + pResult->m_aPoints[2].y) / 2) / 2;

		QuadKnife.m_Count = 0;
		Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionNewQuad>(Map(), Map()->m_SelectedGroup, Map()->m_vSelectedLayers[0]));
	}

	// Render
	Graphics()->TextureClear();
	Graphics()->LinesBegin();

	IGraphics::CLineItem aEdges[] = {
		IGraphics::CLineItem(v[0].x, v[0].y, v[1].x, v[1].y),
		IGraphics::CLineItem(v[1].x, v[1].y, v[2].x, v[2].y),
		IGraphics::CLineItem(v[2].x, v[2].y, v[3].x, v[3].y),
		IGraphics::CLineItem(v[3].x, v[3].y, v[0].x, v[0].y)};

	Graphics()->SetColor(1.f, 0.5f, 0.f, 1.f);
	Graphics()->LinesDraw(aEdges, std::size(aEdges));

	IGraphics::CLineItem aLines[4];
	int LineCount = maximum(QuadKnife.m_Count - 1, 0);

	for(int i = 0; i < LineCount; i++)
		aLines[i] = IGraphics::CLineItem(QuadKnife.m_aPoints[i], QuadKnife.m_aPoints[i + 1]);

	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
	Graphics()->LinesDraw(aLines, LineCount);

	if(ValidPosition)
	{
		if(QuadKnife.m_Count > 0)
		{
			IGraphics::CLineItem LineCurrent(Point, QuadKnife.m_aPoints[QuadKnife.m_Count - 1]);
			Graphics()->LinesDraw(&LineCurrent, 1);
		}

		if(QuadKnife.m_Count == 3)
		{
			IGraphics::CLineItem LineClose(Point, QuadKnife.m_aPoints[0]);
			Graphics()->LinesDraw(&LineClose, 1);
		}
	}

	Graphics()->LinesEnd();
	Graphics()->QuadsBegin();

	IGraphics::CQuadItem aMarkers[4];

	for(int i = 0; i < QuadKnife.m_Count; i++)
		aMarkers[i] = IGraphics::CQuadItem(QuadKnife.m_aPoints[i].x, QuadKnife.m_aPoints[i].y, 5.f * m_MouseWorldScale, 5.f * m_MouseWorldScale);

	Graphics()->SetColor(0.f, 0.f, 1.f, 1.f);
	Graphics()->QuadsDraw(aMarkers, QuadKnife.m_Count);

	if(ValidPosition)
	{
		IGraphics::CQuadItem MarkerCurrent(Point.x, Point.y, 5.f * m_MouseWorldScale, 5.f * m_MouseWorldScale);
		Graphics()->QuadsDraw(&MarkerCurrent, 1);
	}

	Graphics()->QuadsEnd();
}

void CEditor::DoQuadEnvelopes(const CLayerQuads *pLayerQuads)
{
	const std::vector<CQuad> &vQuads = pLayerQuads->m_vQuads;
	if(vQuads.empty())
	{
		return;
	}

	std::vector<std::pair<const CQuad *, CEnvelope *>> vQuadsWithEnvelopes;
	vQuadsWithEnvelopes.reserve(vQuads.size());
	for(const auto &Quad : vQuads)
	{
		if(m_ActiveEnvelopePreview != EEnvelopePreview::ALL &&
			!(m_ActiveEnvelopePreview == EEnvelopePreview::SELECTED && Quad.m_PosEnv == Map()->m_SelectedEnvelope))
		{
			continue;
		}
		if(Quad.m_PosEnv < 0 ||
			Quad.m_PosEnv >= (int)Map()->m_vpEnvelopes.size() ||
			Map()->m_vpEnvelopes[Quad.m_PosEnv]->m_vPoints.empty())
		{
			continue;
		}
		vQuadsWithEnvelopes.emplace_back(&Quad, Map()->m_vpEnvelopes[Quad.m_PosEnv].get());
	}
	if(vQuadsWithEnvelopes.empty())
	{
		return;
	}

	Map()->SelectedGroup()->MapScreen();

	// Draw lines between points
	Graphics()->TextureClear();
	IGraphics::CLineItemBatch LineItemBatch;
	Graphics()->LinesBatchBegin(&LineItemBatch);
	Graphics()->SetColor(ColorRGBA(0.0f, 1.0f, 1.0f, 0.75f));
	for(const auto &[pQuad, pEnvelope] : vQuadsWithEnvelopes)
	{
		if(pEnvelope->m_vPoints.size() < 2)
		{
			continue;
		}

		const CPoint *pPivotPoint = &pQuad->m_aPoints[4];
		const vec2 PivotPoint = vec2(fx2f(pPivotPoint->x), fx2f(pPivotPoint->y));

		for(int PointIndex = 0; PointIndex <= (int)pEnvelope->m_vPoints.size() - 2; PointIndex++)
		{
			const auto &PointStart = pEnvelope->m_vPoints[PointIndex];
			const auto &PointEnd = pEnvelope->m_vPoints[PointIndex + 1];
			const float PointStartTime = PointStart.m_Time.AsSeconds();
			const float PointEndTime = PointEnd.m_Time.AsSeconds();
			const float TimeRange = PointEndTime - PointStartTime;

			int Steps;
			if(PointStart.m_Curvetype == CURVETYPE_BEZIER)
			{
				Steps = std::clamp(round_to_int(TimeRange * 10.0f), 50, 150);
			}
			else
			{
				Steps = 1;
			}
			ColorRGBA StartPosition = PointStart.ColorValue();
			for(int Step = 1; Step <= Steps; Step++)
			{
				ColorRGBA EndPosition;
				if(Step == Steps)
				{
					EndPosition = PointEnd.ColorValue();
				}
				else
				{
					const float SectionEndTime = PointStartTime + TimeRange * (Step / (float)Steps);
					EndPosition = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
					pEnvelope->Eval(SectionEndTime, EndPosition, 2);
				}

				const vec2 Pos0 = PivotPoint + vec2(StartPosition.r, StartPosition.g);
				const vec2 Pos1 = PivotPoint + vec2(EndPosition.r, EndPosition.g);
				const IGraphics::CLineItem Item = IGraphics::CLineItem(Pos0, Pos1);
				Graphics()->LinesBatchDraw(&LineItemBatch, &Item, 1);

				StartPosition = EndPosition;
			}
		}
	}
	Graphics()->LinesBatchEnd(&LineItemBatch);

	// Draw quads at points
	if(pLayerQuads->m_Image >= 0 && pLayerQuads->m_Image < (int)Map()->m_vpImages.size())
	{
		Graphics()->TextureSet(Map()->m_vpImages[pLayerQuads->m_Image]->m_Texture);
	}
	else
	{
		Graphics()->TextureClear();
	}
	Graphics()->QuadsBegin();
	for(const auto &[pQuad, pEnvelope] : vQuadsWithEnvelopes)
	{
		for(size_t PointIndex = 0; PointIndex < pEnvelope->m_vPoints.size(); PointIndex++)
		{
			const CEnvPoint_runtime &EnvPoint = pEnvelope->m_vPoints[PointIndex];
			const vec2 Offset = vec2(fx2f(EnvPoint.m_aValues[0]), fx2f(EnvPoint.m_aValues[1]));
			const float Rotation = fx2f(EnvPoint.m_aValues[2]) / 180.0f * pi;

			const float Alpha = (Map()->m_SelectedQuadEnvelope == pQuad->m_PosEnv && Map()->IsEnvPointSelected(PointIndex)) ? 0.65f : 0.35f;
			Graphics()->SetColor4(
				ColorRGBA(pQuad->m_aColors[0].r, pQuad->m_aColors[0].g, pQuad->m_aColors[0].b, pQuad->m_aColors[0].a).Multiply(1.0f / 255.0f).WithMultipliedAlpha(Alpha),
				ColorRGBA(pQuad->m_aColors[1].r, pQuad->m_aColors[1].g, pQuad->m_aColors[1].b, pQuad->m_aColors[1].a).Multiply(1.0f / 255.0f).WithMultipliedAlpha(Alpha),
				ColorRGBA(pQuad->m_aColors[3].r, pQuad->m_aColors[3].g, pQuad->m_aColors[3].b, pQuad->m_aColors[3].a).Multiply(1.0f / 255.0f).WithMultipliedAlpha(Alpha),
				ColorRGBA(pQuad->m_aColors[2].r, pQuad->m_aColors[2].g, pQuad->m_aColors[2].b, pQuad->m_aColors[2].a).Multiply(1.0f / 255.0f).WithMultipliedAlpha(Alpha));

			const CPoint *pPoints;
			CPoint aRotated[4];
			if(Rotation != 0.0f)
			{
				std::copy_n(pQuad->m_aPoints, std::size(aRotated), aRotated);
				for(auto &Point : aRotated)
				{
					Rotate(&pQuad->m_aPoints[4], &Point, Rotation);
				}
				pPoints = aRotated;
			}
			else
			{
				pPoints = pQuad->m_aPoints;
			}
			Graphics()->QuadsSetSubsetFree(
				fx2f(pQuad->m_aTexcoords[0].x), fx2f(pQuad->m_aTexcoords[0].y),
				fx2f(pQuad->m_aTexcoords[1].x), fx2f(pQuad->m_aTexcoords[1].y),
				fx2f(pQuad->m_aTexcoords[2].x), fx2f(pQuad->m_aTexcoords[2].y),
				fx2f(pQuad->m_aTexcoords[3].x), fx2f(pQuad->m_aTexcoords[3].y));

			const IGraphics::CFreeformItem Freeform(
				fx2f(pPoints[0].x) + Offset.x, fx2f(pPoints[0].y) + Offset.y,
				fx2f(pPoints[1].x) + Offset.x, fx2f(pPoints[1].y) + Offset.y,
				fx2f(pPoints[2].x) + Offset.x, fx2f(pPoints[2].y) + Offset.y,
				fx2f(pPoints[3].x) + Offset.x, fx2f(pPoints[3].y) + Offset.y);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}
	}
	Graphics()->QuadsEnd();

	// Draw quad envelope point handles
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	for(const auto &[pQuad, pEnvelope] : vQuadsWithEnvelopes)
	{
		for(size_t PointIndex = 0; PointIndex < pEnvelope->m_vPoints.size(); PointIndex++)
		{
			DoQuadEnvPoint(pQuad, pEnvelope, pQuad - vQuads.data(), PointIndex);
		}
	}
	Graphics()->QuadsEnd();
}

void CEditor::DoQuadEnvPoint(const CQuad *pQuad, CEnvelope *pEnvelope, int QuadIndex, int PointIndex)
{
	CEnvPoint_runtime *pPoint = &pEnvelope->m_vPoints[PointIndex];
	const vec2 Center = vec2(fx2f(pQuad->m_aPoints[4].x) + fx2f(pPoint->m_aValues[0]), fx2f(pQuad->m_aPoints[4].y) + fx2f(pPoint->m_aValues[1]));
	const bool IgnoreGrid = Input()->AltIsPressed();

	if(Ui()->CheckActiveItem(pPoint) && Map()->m_CurrentQuadIndex == QuadIndex)
	{
		if(m_MouseDeltaWorld != vec2(0.0f, 0.0f))
		{
			if(m_QuadEnvelopePointOperation == EQuadEnvelopePointOperation::MOVE)
			{
				vec2 Pos = Ui()->MouseWorldPos();
				if(MapView()->MapGrid()->IsEnabled() && !IgnoreGrid)
				{
					MapView()->MapGrid()->SnapToGrid(Pos);
				}
				pPoint->m_aValues[0] = f2fx(Pos.x) - pQuad->m_aPoints[4].x;
				pPoint->m_aValues[1] = f2fx(Pos.y) - pQuad->m_aPoints[4].y;
			}
			else if(m_QuadEnvelopePointOperation == EQuadEnvelopePointOperation::ROTATE)
			{
				pPoint->m_aValues[2] += 10 * Ui()->MouseDeltaX();
			}
		}

		if(!Ui()->MouseButton(0))
		{
			Ui()->DisableMouseLock();
			m_QuadEnvelopePointOperation = EQuadEnvelopePointOperation::NONE;
			Ui()->SetActiveItem(nullptr);
		}

		Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
	}
	else if(Ui()->HotItem() == pPoint && Map()->m_CurrentQuadIndex == QuadIndex)
	{
		Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
		str_copy(m_aTooltip, "Left mouse button to move. Hold ctrl to rotate. Hold alt to ignore grid.");

		if(Ui()->MouseButton(0))
		{
			if(Input()->ModifierIsPressed())
			{
				Ui()->EnableMouseLock(pPoint);
				m_QuadEnvelopePointOperation = EQuadEnvelopePointOperation::ROTATE;
			}
			else
			{
				m_QuadEnvelopePointOperation = EQuadEnvelopePointOperation::MOVE;
			}
			Map()->SelectQuad(QuadIndex);
			Map()->SelectEnvPoint(PointIndex);
			Map()->m_SelectedQuadEnvelope = pQuad->m_PosEnv;
			Ui()->SetActiveItem(pPoint);
		}
		else
		{
			Map()->DeselectEnvPoints();
			Map()->m_SelectedQuadEnvelope = -1;
		}
	}
	else
	{
		Graphics()->SetColor(ColorRGBA(0.0f, 1.0f, 1.0f, 1.0f));
	}

	IGraphics::CQuadItem QuadItem(Center.x, Center.y, 5.0f * m_MouseWorldScale, 5.0f * m_MouseWorldScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CEditor::DoMapEditor(CUIRect View)
{
	// render all good stuff
	if(!m_ShowPicker)
	{
		MapView()->RenderEditorMap();
	}
	else
	{
		// fix aspect ratio of the image in the picker
		float Max = minimum(View.w, View.h);
		View.w = View.h = Max;
	}

	const bool Inside = Ui()->MouseInside(&View);

	// fetch mouse position
	float wx = Ui()->MouseWorldX();
	float wy = Ui()->MouseWorldY();
	float mx = Ui()->MouseX();
	float my = Ui()->MouseY();

	static float s_StartWx = 0;
	static float s_StartWy = 0;
	static float s_SelectRotateStartMx = 0.0f;
	static float s_SelectRotateStartAngle = 0.0f;

	enum
	{
		OP_NONE = 0,
		OP_BRUSH_GRAB,
		OP_BRUSH_DRAW,
		OP_BRUSH_PAINT,
		OP_BRUSH_FILL,
		OP_SHAPE_DRAW,
		OP_LINE_DRAG,
		OP_SYM_DRAG_H,
		OP_SYM_DRAG_V,
		OP_SYM_DRAG_CENTER,
		OP_FADE_DRAG,
		OP_PAN_WORLD,
		OP_PAN_EDITOR,
		OP_SELECT_FREEHAND,
		OP_SELECT_ROTATE,
		OP_THREED_DRAG,
		OP_SHAPE_GHOST_DRAG,
	};

	// remap the screen so it can display the whole tileset
	if(m_ShowPicker)
	{
		CUIRect Screen = *Ui()->Screen();
		float Size = 32.0f * 16.0f;
		float w = Size * (Screen.w / View.w);
		float h = Size * (Screen.h / View.h);
		float x = -(View.x / Screen.w) * w;
		float y = -(View.y / Screen.h) * h;
		wx = x + w * mx / Screen.w;
		wy = y + h * my / Screen.h;
		std::shared_ptr<CLayerTiles> pTileLayer = std::static_pointer_cast<CLayerTiles>(Map()->SelectedLayerType(0, LAYERTYPE_TILES));
		if(pTileLayer)
		{
			Graphics()->MapScreen(x, y, x + w, y + h);
			m_pTilesetPicker->m_Image = pTileLayer->m_Image;
			if(m_BrushColorEnabled)
			{
				m_pTilesetPicker->m_Color = pTileLayer->m_Color;
				m_pTilesetPicker->m_Color.a = 255;
			}
			else
			{
				m_pTilesetPicker->m_Color = {255, 255, 255, 255};
			}

			m_pTilesetPicker->m_HasGame = pTileLayer->m_HasGame;
			m_pTilesetPicker->m_HasTele = pTileLayer->m_HasTele;
			m_pTilesetPicker->m_HasSpeedup = pTileLayer->m_HasSpeedup;
			m_pTilesetPicker->m_HasFront = pTileLayer->m_HasFront;
			m_pTilesetPicker->m_HasSwitch = pTileLayer->m_HasSwitch;
			m_pTilesetPicker->m_HasTune = pTileLayer->m_HasTune;

			m_pTilesetPicker->Render(true);

			if(m_ShowTileInfo != SHOW_TILE_OFF)
				m_pTilesetPicker->ShowInfo();
		}
		else
		{
			std::shared_ptr<CLayerQuads> pQuadLayer = std::static_pointer_cast<CLayerQuads>(Map()->SelectedLayerType(0, LAYERTYPE_QUADS));
			if(pQuadLayer)
			{
				m_pQuadsetPicker->m_Image = pQuadLayer->m_Image;
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[0].x = f2fx(View.x);
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[0].y = f2fx(View.y);
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[1].x = f2fx((View.x + View.w));
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[1].y = f2fx(View.y);
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[2].x = f2fx(View.x);
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[2].y = f2fx((View.y + View.h));
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[3].x = f2fx((View.x + View.w));
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[3].y = f2fx((View.y + View.h));
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[4].x = f2fx((View.x + View.w / 2));
				m_pQuadsetPicker->m_vQuads[0].m_aPoints[4].y = f2fx((View.y + View.h / 2));
				m_pQuadsetPicker->Render();
			}
		}
	}

	static int s_Operation = OP_NONE;

	// draw layer borders
	std::pair<int, std::shared_ptr<CLayer>> apEditLayers[128];
	size_t NumEditLayers = 0;

	if(m_ShowPicker && Map()->SelectedLayer(0) && Map()->SelectedLayer(0)->m_Type == LAYERTYPE_TILES)
	{
		apEditLayers[0] = {0, m_pTilesetPicker};
		NumEditLayers++;
	}
	else if(m_ShowPicker)
	{
		apEditLayers[0] = {0, m_pQuadsetPicker};
		NumEditLayers++;
	}
	else
	{
		// pick a type of layers to edit, preferring Tiles layers.
		int EditingType = -1;
		for(size_t i = 0; i < Map()->m_vSelectedLayers.size(); i++)
		{
			std::shared_ptr<CLayer> pLayer = Map()->SelectedLayer(i);
			if(pLayer && (EditingType == -1 || pLayer->m_Type == LAYERTYPE_TILES))
			{
				EditingType = pLayer->m_Type;
				if(EditingType == LAYERTYPE_TILES)
					break;
			}
		}
		for(size_t i = 0; i < Map()->m_vSelectedLayers.size() && NumEditLayers < 128; i++)
		{
			apEditLayers[NumEditLayers] = {Map()->m_vSelectedLayers[i], Map()->SelectedLayerType(i, EditingType)};
			if(apEditLayers[NumEditLayers].second)
			{
				NumEditLayers++;
			}
		}

		MapView()->RenderGroupBorder();
		MapView()->MapGrid()->OnRender(View);
	}

	const bool ShouldPan = Ui()->HotItem() == &m_MapEditorId && ((Input()->ModifierIsPressed() && Ui()->MouseButton(0)) || Ui()->MouseButton(2));
	if(m_pContainerPanned == &m_MapEditorId)
	{
		// do panning
		if(ShouldPan)
		{
			if(Input()->ShiftIsPressed())
				s_Operation = OP_PAN_EDITOR;
			else
				s_Operation = OP_PAN_WORLD;
			Ui()->SetActiveItem(&m_MapEditorId);
		}
		else
			s_Operation = OP_NONE;

		if(s_Operation == OP_PAN_WORLD)
			MapView()->OffsetWorld(-Ui()->MouseDelta() * m_MouseWorldScale);
		else if(s_Operation == OP_PAN_EDITOR)
			MapView()->OffsetEditor(-Ui()->MouseDelta() * m_MouseWorldScale);

		if(s_Operation == OP_NONE)
			m_pContainerPanned = nullptr;
	}

	if(Inside)
	{
		Ui()->SetHotItem(&m_MapEditorId);

		// do global operations like pan and zoom
		if(Ui()->CheckActiveItem(nullptr) && (Ui()->MouseButton(0) || Ui()->MouseButton(2)))
		{
			s_StartWx = wx;
			s_StartWy = wy;

			if(ShouldPan && m_pContainerPanned == nullptr)
				m_pContainerPanned = &m_MapEditorId;
		}

		// brush editing
		if(Ui()->HotItem() == &m_MapEditorId)
		{
			if(m_ShowPicker)
			{
				std::shared_ptr<CLayer> pLayer = Map()->SelectedLayer(0);
				int Layer;
				if(pLayer == Map()->m_pGameLayer)
					Layer = LAYER_GAME;
				else if(pLayer == Map()->m_pFrontLayer)
					Layer = LAYER_FRONT;
				else if(pLayer == Map()->m_pSwitchLayer)
					Layer = LAYER_SWITCH;
				else if(pLayer == Map()->m_pTeleLayer)
					Layer = LAYER_TELE;
				else if(pLayer == Map()->m_pSpeedupLayer)
					Layer = LAYER_SPEEDUP;
				else if(pLayer == Map()->m_pTuneLayer)
					Layer = LAYER_TUNE;
				else
					Layer = NUM_LAYERS;

				CExplanations::EGametype ExplanationGametype;
				if(m_SelectEntitiesImage == "DDNet")
					ExplanationGametype = CExplanations::EGametype::DDNET;
				else if(m_SelectEntitiesImage == "FNG")
					ExplanationGametype = CExplanations::EGametype::FNG;
				else if(m_SelectEntitiesImage == "Race")
					ExplanationGametype = CExplanations::EGametype::RACE;
				else if(m_SelectEntitiesImage == "Vanilla")
					ExplanationGametype = CExplanations::EGametype::VANILLA;
				else if(m_SelectEntitiesImage == "blockworlds")
					ExplanationGametype = CExplanations::EGametype::BLOCKWORLDS;
				else
					ExplanationGametype = CExplanations::EGametype::NONE;

				if(Layer != NUM_LAYERS)
				{
					const char *pExplanation = CExplanations::Explain(ExplanationGametype, (int)wx / 32 + (int)wy / 32 * 16, Layer);
					if(pExplanation)
						str_copy(m_aTooltip, pExplanation);
				}
			}
			else if(m_pBrush->IsEmpty() && Map()->SelectedLayerType(0, LAYERTYPE_QUADS) != nullptr)
				str_copy(m_aTooltip, "Use left mouse button to drag and create a brush. Hold shift to select multiple quads. Press R to rotate selected quads. Use ctrl+right click to select layer.");
			else if(m_pBrush->IsEmpty())
			{
				if(g_Config.m_EdLayerSelector)
					str_copy(m_aTooltip, "Use left mouse button to drag and create a brush. Use ctrl+right click to select layer of hovered tile.");
				else
					str_copy(m_aTooltip, "Use left mouse button to drag and create a brush.");
			}
			else
			{
				// Alt behavior handled in CEditor::MouseAxisLock
				if(m_SelectTool && !m_vSelectOrigTiles.empty())
					str_copy(m_aTooltip, "Left click to stamp selection. Right-click drag to rotate at any angle. R/T to step-rotate.");
				else
					str_copy(m_aTooltip, "Use left mouse button to paint with the brush. Right click to clear the brush. Hold Alt to lock the mouse movement to a single axis.");
			}

			if(Ui()->CheckActiveItem(&m_MapEditorId))
			{
				CUIRect r;
				r.x = s_StartWx;
				r.y = s_StartWy;
				r.w = wx - s_StartWx;
				r.h = wy - s_StartWy;
				if(r.w < 0)
				{
					r.x += r.w;
					r.w = -r.w;
				}

				if(r.h < 0)
				{
					r.y += r.h;
					r.h = -r.h;
				}

				if(s_Operation == OP_BRUSH_DRAW)
				{
					if(!m_pBrush->IsEmpty())
					{
						// draw with brush
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k % m_pBrush->m_vpLayers.size();
							if(apEditLayers[k].second->m_Type == m_pBrush->m_vpLayers[BrushIndex]->m_Type)
							{
								if(apEditLayers[k].second->m_Type == LAYERTYPE_TILES)
								{
									std::shared_ptr<CLayerTiles> pLayer = std::static_pointer_cast<CLayerTiles>(apEditLayers[k].second);
									std::shared_ptr<CLayerTiles> pBrushLayer = std::static_pointer_cast<CLayerTiles>(m_pBrush->m_vpLayers[BrushIndex]);

									if((!pLayer->m_HasTele || pBrushLayer->m_HasTele) && (!pLayer->m_HasSpeedup || pBrushLayer->m_HasSpeedup) && (!pLayer->m_HasFront || pBrushLayer->m_HasFront) && (!pLayer->m_HasGame || pBrushLayer->m_HasGame) && (!pLayer->m_HasSwitch || pBrushLayer->m_HasSwitch) && (!pLayer->m_HasTune || pBrushLayer->m_HasTune))
										pLayer->BrushDraw(pBrushLayer.get(), vec2(wx, wy));
								}
								else
								{
									apEditLayers[k].second->BrushDraw(m_pBrush->m_vpLayers[BrushIndex].get(), vec2(wx, wy));
								}
							}
						}
					}
				}
				else if(s_Operation == OP_BRUSH_GRAB)
				{
					if(!Ui()->MouseButton(0))
					{
						std::shared_ptr<CLayerQuads> pQuadLayer = std::static_pointer_cast<CLayerQuads>(Map()->SelectedLayerType(0, LAYERTYPE_QUADS));
						if(Input()->ShiftIsPressed() && pQuadLayer)
						{
							Map()->DeselectQuads();
							for(size_t i = 0; i < pQuadLayer->m_vQuads.size(); i++)
							{
								const CQuad &Quad = pQuadLayer->m_vQuads[i];
								vec2 Position = vec2(fx2f(Quad.m_aPoints[4].x), fx2f(Quad.m_aPoints[4].y));
								if(r.Inside(Position) && !Map()->IsQuadSelected(i))
									Map()->ToggleSelectQuad(i);
							}
						}
						else
						{
							// TODO: do all layers
							int Grabs = 0;
							for(size_t k = 0; k < NumEditLayers; k++)
								Grabs += apEditLayers[k].second->BrushGrab(m_pBrush.get(), r);
							if(Grabs == 0)
								m_pBrush->Clear();
							else if(m_SelectTool)
							{
								m_SelectTotalAngle = 0.0f;
								SelectToolSaveOrig();
							}

							Map()->DeselectQuads();
							Map()->DeselectQuadPoints();
						}
					}
					else
					{
						for(size_t k = 0; k < NumEditLayers; k++)
							apEditLayers[k].second->BrushSelecting(r);
						Ui()->MapScreen();
					}
				}
				else if(s_Operation == OP_SELECT_FREEHAND)
				{
					if(!Ui()->MouseButton(0))
					{
						if(m_vSelectPath.size() >= 3)
						{
							m_pBrush->Clear();
							m_SelectTotalAngle = 0.0f;
							int Grabs = 0;
							for(size_t k = 0; k < NumEditLayers; k++)
							{
								if(apEditLayers[k].second->m_Type == LAYERTYPE_TILES)
								{
									std::shared_ptr<CLayerTiles> pTileLayer = std::static_pointer_cast<CLayerTiles>(apEditLayers[k].second);
									Grabs += pTileLayer->BrushGrabFreehand(m_pBrush.get(), m_vSelectPath);
								}
							}
							if(Grabs == 0)
								m_pBrush->Clear();
							else
								SelectToolSaveOrig();
						}
						m_vSelectPath.clear();
					}
					else
					{
						// Accumulate path and render polygon outline
						m_vSelectPath.emplace_back(wx, wy);
						Graphics()->TextureClear();
						Graphics()->LinesBegin();
						Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.9f);
						for(size_t i = 1; i < m_vSelectPath.size(); i++)
						{
							IGraphics::CLineItem Line(m_vSelectPath[i - 1].x, m_vSelectPath[i - 1].y,
								m_vSelectPath[i].x, m_vSelectPath[i].y);
							Graphics()->LinesDraw(&Line, 1);
						}
						// Close preview
						if(m_vSelectPath.size() >= 2)
						{
							IGraphics::CLineItem CloseLine(m_vSelectPath.back().x, m_vSelectPath.back().y,
								m_vSelectPath[0].x, m_vSelectPath[0].y);
							Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
							Graphics()->LinesDraw(&CloseLine, 1);
						}
						Graphics()->LinesEnd();
						Ui()->MapScreen();
					}
				}
				else if(s_Operation == OP_SELECT_ROTATE)
				{
					if(!Ui()->MouseButton(1))
					{
						// rotation committed — nothing special needed
					}
					else if(!m_vSelectOrigTiles.empty())
					{
						float Delta = mx - s_SelectRotateStartMx;
						m_SelectTotalAngle = s_SelectRotateStartAngle + Delta * (pi / 180.0f);
						for(auto &pLayer : m_pBrush->m_vpLayers)
							if(pLayer->m_Type == LAYERTYPE_TILES)
							{
								std::static_pointer_cast<CLayerTiles>(pLayer)->BrushRotateArbitrary(
									m_vSelectOrigTiles, m_SelectOrigW, m_SelectOrigH, m_SelectTotalAngle);
								break;
							}
						// draw angle label next to cursor
						Ui()->MapScreen();
						char aBuf[32];
						str_format(aBuf, sizeof(aBuf), "%.1f deg", m_SelectTotalAngle * 180.0f / pi);
						TextRender()->Text(mx + 10.0f, my - 14.0f, 12.0f, aBuf, -1.0f);
					}
				}
				else if(s_Operation == OP_BRUSH_PAINT)
				{
					if(!Ui()->MouseButton(0))
					{
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k;
							if(m_pBrush->m_vpLayers.size() != NumEditLayers)
								BrushIndex = 0;
							std::shared_ptr<CLayer> pBrush = m_pBrush->IsEmpty() ? nullptr : m_pBrush->m_vpLayers[BrushIndex];
							apEditLayers[k].second->FillSelection(m_pBrush->IsEmpty(), pBrush.get(), r);
						}
						std::shared_ptr<IEditorAction> Action = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
						Map()->m_EditorHistory.RecordAction(Action);
					}
					else
					{
						for(size_t k = 0; k < NumEditLayers; k++)
							apEditLayers[k].second->BrushSelecting(r);
						Ui()->MapScreen();
					}
				}
				else if(s_Operation == OP_SYM_DRAG_H)
				{
					m_SymmetryCenter.x = std::round(wx / 32.0f) * 32.0f;
				}
				else if(s_Operation == OP_SYM_DRAG_V)
				{
					m_SymmetryCenter.y = std::round(wy / 32.0f) * 32.0f;
				}
				else if(s_Operation == OP_SYM_DRAG_CENTER)
				{
					m_SymmetryCenter = vec2(std::round(wx / 32.0f) * 32.0f, std::round(wy / 32.0f) * 32.0f);
				}
				else if(s_Operation == OP_FADE_DRAG)
				{
					FadeToolApplyDrag(wx, wy);
				}
				else if(s_Operation == OP_LINE_DRAG)
				{
					if(!m_LineDragMoved)
					{
						const float Threshold = 4.0f;
						const float Dx = wx - m_LineDragStartMouse.x;
						const float Dy = wy - m_LineDragStartMouse.y;
						if(Dx * Dx + Dy * Dy > Threshold * Threshold)
							m_LineDragMoved = true;
					}
					if(m_LineDragMoved && m_LineActiveNode >= 0 && m_LineActiveNode < (int)m_vLineNodes.size())
					{
						const float Dx = wx - m_LineDragStartMouse.x;
						const float Dy = wy - m_LineDragStartMouse.y;
						if(m_LineDragKind == LINE_DRAG_HANDLE_IN && m_LineActiveNode < (int)m_vLineHandleIn.size())
						{
							m_vLineHandleIn[m_LineActiveNode] = m_LineDragStartHandleIn + vec2(Dx, Dy);
						}
						else if(m_LineDragKind == LINE_DRAG_HANDLE_OUT && m_LineActiveNode < (int)m_vLineHandleOut.size())
						{
							m_vLineHandleOut[m_LineActiveNode] = m_LineDragStartHandleOut + vec2(Dx, Dy);
						}
						else
						{
							m_vLineNodes[m_LineActiveNode] = vec2(wx, wy);
							const int Nn = (int)m_vLineNodes.size();
							const int Idx = m_LineActiveNode;
							if(Idx > 0)
								LineRecomputeAutoHandles(Idx - 1);
							if(Idx + 1 < Nn)
								LineRecomputeAutoHandles(Idx + 1);
						}
					}
				}
				else if(s_Operation == OP_THREED_DRAG)
				{
					if(m_ThreeDDragNode >= 0)
					{
						const float Dx = wx - m_ThreeDDragStartMouse.x;
						const float Dy = wy - m_ThreeDDragStartMouse.y;
						if(m_ThreeDDragNode == 0) // center
							m_ThreeDCenter = m_ThreeDDragStartCenter + vec2(Dx, Dy);
						else if(m_ThreeDDragNode == 1) // ScaleX
							m_ThreeDScaleX = maximum(32.0f, m_ThreeDDragStartScaleX + Dx);
						else if(m_ThreeDDragNode == 2) // ScaleY
							m_ThreeDScaleY = maximum(32.0f, m_ThreeDDragStartScaleY + Dy);
						else if(m_ThreeDDragNode == 3) // ScaleZ
							m_ThreeDScaleZ = maximum(32.0f, m_ThreeDDragStartScaleZ + Dx);
						else if(m_ThreeDDragNode == 4) // Rotation: horizontal=Ry, vertical=Rx
						{
							const float RotSens = 0.3f / m_MouseWorldScale;
							m_ThreeDRyDeg = m_ThreeDDragStartRy + Dx * RotSens;
							m_ThreeDRxDeg = m_ThreeDDragStartRx + Dy * RotSens;
						}
					}
				}
				else if(s_Operation == OP_SHAPE_GHOST_DRAG)
				{
					if(m_ShapeGhostDragNode >= 0)
					{
						const float Dx = wx - m_ShapeGhostDragStartMouse.x;
						const float Dy = wy - m_ShapeGhostDragStartMouse.y;
						const int TileDx = (int)std::floor(Dx / 32.0f + 0.5f);
						const int TileDy = (int)std::floor(Dy / 32.0f + 0.5f);
						const int TileX = (int)std::floor(wx / 32.0f);
						const int TileY = (int)std::floor(wy / 32.0f);
						if(m_ShapeGhostDragNode == 0) // center — move whole shape
						{
							const int W = m_ShapeGhostDragStartX2 - m_ShapeGhostDragStartX1;
							const int H = m_ShapeGhostDragStartY2 - m_ShapeGhostDragStartY1;
							m_ShapeGhostX1 = m_ShapeGhostDragStartX1 + TileDx;
							m_ShapeGhostY1 = m_ShapeGhostDragStartY1 + TileDy;
							m_ShapeGhostX2 = m_ShapeGhostX1 + W;
							m_ShapeGhostY2 = m_ShapeGhostY1 + H;
						}
						else if(m_ShapeGhostDragNode == 1) // top-left
						{
							m_ShapeGhostX1 = std::min(TileX, m_ShapeGhostDragStartX2);
							m_ShapeGhostX2 = std::max(TileX, m_ShapeGhostDragStartX2);
							m_ShapeGhostY1 = std::min(TileY, m_ShapeGhostDragStartY2);
							m_ShapeGhostY2 = std::max(TileY, m_ShapeGhostDragStartY2);
						}
						else if(m_ShapeGhostDragNode == 2) // top-right
						{
							m_ShapeGhostX1 = std::min(TileX, m_ShapeGhostDragStartX1);
							m_ShapeGhostX2 = std::max(TileX, m_ShapeGhostDragStartX1);
							m_ShapeGhostY1 = std::min(TileY, m_ShapeGhostDragStartY2);
							m_ShapeGhostY2 = std::max(TileY, m_ShapeGhostDragStartY2);
						}
						else if(m_ShapeGhostDragNode == 3) // bottom-left
						{
							m_ShapeGhostX1 = std::min(TileX, m_ShapeGhostDragStartX2);
							m_ShapeGhostX2 = std::max(TileX, m_ShapeGhostDragStartX2);
							m_ShapeGhostY1 = std::min(TileY, m_ShapeGhostDragStartY1);
							m_ShapeGhostY2 = std::max(TileY, m_ShapeGhostDragStartY1);
						}
						else if(m_ShapeGhostDragNode == 4) // bottom-right
						{
							m_ShapeGhostX1 = std::min(TileX, m_ShapeGhostDragStartX1);
							m_ShapeGhostX2 = std::max(TileX, m_ShapeGhostDragStartX1);
							m_ShapeGhostY1 = std::min(TileY, m_ShapeGhostDragStartY1);
							m_ShapeGhostY2 = std::max(TileY, m_ShapeGhostDragStartY1);
						}
					}
				}
				else if(s_Operation == OP_SHAPE_DRAW)
				{
					int X1 = (int)floorf(s_StartWx / 32.0f);
					int Y1 = (int)floorf(s_StartWy / 32.0f);
					int X2 = (int)floorf(wx / 32.0f);
					int Y2 = (int)floorf(wy / 32.0f);

					if(Input()->AltIsPressed())
					{
						// Center-out: click point is the center of the shape.
						const int Cx = X1;
						const int Cy = Y1;
						int HalfX = std::abs(X2 - Cx);
						int HalfY = std::abs(Y2 - Cy);
						if(Input()->ShiftIsPressed())
						{
							const int Half = std::max(HalfX, HalfY);
							HalfX = HalfY = Half;
						}
						X1 = Cx - HalfX;
						X2 = Cx + HalfX;
						Y1 = Cy - HalfY;
						Y2 = Cy + HalfY;
					}
					else if(Input()->ShiftIsPressed())
					{
						const int Dx = X2 - X1;
						const int Dy = Y2 - Y1;
						const int AbsDx = std::abs(Dx);
						const int AbsDy = std::abs(Dy);
						const int Side = std::max(AbsDx, AbsDy);
						X2 = X1 + (Dx >= 0 ? Side : -Side);
						Y2 = Y1 + (Dy >= 0 ? Side : -Side);
					}

					if(!Ui()->MouseButton(0))
					{
						// Save shape to persistent ghost instead of stamping immediately
						m_ShapeGhostX1 = std::min(X1, X2);
						m_ShapeGhostY1 = std::min(Y1, Y2);
						m_ShapeGhostX2 = std::max(X1, X2);
						m_ShapeGhostY2 = std::max(Y1, Y2);
						m_ShapeGhostInit = true;
					}
					else
					{
						// Live preview during drag
						std::vector<std::pair<int, int>> PreviewCells;
						CLayerTiles::CollectShapeCells(X1, Y1, X2, Y2, m_ShapeKind, m_ShapeFilled, m_ShapeThickness, PreviewCells, m_ShapeNgonSides);
						Graphics()->TextureClear();
						Graphics()->QuadsBegin();
						Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
						for(const auto &Cell : PreviewCells)
						{
							IGraphics::CQuadItem QuadItem(Cell.first * 32.0f, Cell.second * 32.0f, 32.0f, 32.0f);
							Graphics()->QuadsDrawTL(&QuadItem, 1);
						}
						Graphics()->QuadsEnd();
					}
				}
			}
			else
			{
				if(m_FadeTool && !m_ShowPicker)
				{
					if(Ui()->MouseButtonClicked(1))
					{
						const int Hover = FadeToolHitTest(wx, wy);
						if(Hover >= 0 && Hover < (int)m_vFadeNodes.size())
						{
							if((int)m_vFadeNodes.size() > 2)
							{
								m_vFadeNodes.erase(m_vFadeNodes.begin() + Hover);
								if(Hover < (int)m_vFadeHandleIn.size())
									m_vFadeHandleIn.erase(m_vFadeHandleIn.begin() + Hover);
								if(Hover < (int)m_vFadeHandleOut.size())
									m_vFadeHandleOut.erase(m_vFadeHandleOut.begin() + Hover);
								m_FadeActiveHandle = -1;
								m_FadeDragKind = FADE_DRAG_NONE;
								// Refresh auto handles for neighbors of the removed node.
								const int N = (int)m_vFadeNodes.size();
								for(int Off = -1; Off <= 1; ++Off)
								{
									const int Idx = Hover + Off;
									if(Idx >= 0 && Idx < N)
										FadeRecomputeAutoHandles(Idx);
								}
							}
						}
						// Right-click on empty space is a no-op for the fade tool — Esc or the toolbar
						// toggle is the way to cancel.
					}
				}
				else if(m_LineTool && !m_ShowPicker)
				{
					if(Ui()->MouseButtonClicked(1))
					{
						const int Hover = LineToolFindHoveredNode(wx, wy);
						if(Hover >= 0)
						{
							m_vLineNodes.erase(m_vLineNodes.begin() + Hover);
							if(Hover < (int)m_vLineNodeKinds.size())
								m_vLineNodeKinds.erase(m_vLineNodeKinds.begin() + Hover);
							if(Hover < (int)m_vLineHandleIn.size())
								m_vLineHandleIn.erase(m_vLineHandleIn.begin() + Hover);
							if(Hover < (int)m_vLineHandleOut.size())
								m_vLineHandleOut.erase(m_vLineHandleOut.begin() + Hover);
							m_LineActiveNode = -1;
							m_LineHoverNode = -1;
							m_LineDragKind = LINE_DRAG_NONE;
							// Adjust selection: deselect if the deleted node was selected,
							// otherwise shift the index down if it was after the deleted one.
							if(m_LineSelectedNode == Hover)
								m_LineSelectedNode = -1;
							else if(m_LineSelectedNode > Hover)
								--m_LineSelectedNode;
							const int Nn = (int)m_vLineNodes.size();
							for(int Off = -1; Off <= 1; ++Off)
							{
								const int Idx = Hover + Off;
								if(Idx >= 0 && Idx < Nn)
									LineRecomputeAutoHandles(Idx);
							}
							if((int)m_vLineNodes.size() < 3)
								m_LineClosed = false;
						}
						// Right-click on empty space is a no-op for the line tool — Esc cancels.
					}
				}
				else if(m_SelectTool && !m_vSelectOrigTiles.empty() && !m_ShowPicker && Ui()->MouseButton(1) && s_Operation == OP_NONE)
				{
					Ui()->SetActiveItem(&m_MapEditorId);
					s_Operation = OP_SELECT_ROTATE;
					s_SelectRotateStartMx = mx;
					s_SelectRotateStartAngle = m_SelectTotalAngle;
				}
				else if(Ui()->MouseButton(1))
				{
					if(!m_SelectTool)
						m_pBrush->Clear();
				}

				if(m_LineTool && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
				{
					if(Input()->KeyPress(KEY_RETURN) || Input()->KeyPress(KEY_KP_ENTER))
						LineToolCommit(m_LineClosed);
					else if(Input()->KeyPress(KEY_ESCAPE))
						LineToolCancel();
					// C: cycle the selected node's smooth↔sharp kind.
					else if(Input()->KeyPress(KEY_C) && !Input()->ModifierIsPressed() && !Input()->ShiftIsPressed() && !Ui()->IsPopupOpen() && m_LineSelectedNode >= 0 && m_LineSelectedNode < (int)m_vLineNodeKinds.size())
					{
						m_vLineNodeKinds[m_LineSelectedNode] = (m_vLineNodeKinds[m_LineSelectedNode] + 1) % 2;
						const int N = (int)m_vLineNodes.size();
						for(int Off = -1; Off <= 1; ++Off)
						{
							const int Idx = m_LineSelectedNode + Off;
							if(Idx >= 0 && Idx < N)
								LineRecomputeAutoHandles(Idx);
						}
					}
				}

				if(m_FadeTool && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
				{
					if(Input()->KeyPress(KEY_RETURN) || Input()->KeyPress(KEY_KP_ENTER))
						FadeToolCommit();
					else if(Input()->KeyPress(KEY_ESCAPE))
						FadeToolCancel();
				}

				if(m_ThreeDTool && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
				{
					if(Input()->KeyPress(KEY_RETURN) || Input()->KeyPress(KEY_KP_ENTER))
						ThreeDToolCommit();
					else if(Input()->KeyPress(KEY_ESCAPE))
					{
						ThreeDToolCancel();
						m_ThreeDTool = false;
					}
				}

				if(m_ShapeTool && m_ShapeGhostInit && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
				{
					if(Input()->KeyPress(KEY_RETURN) || Input()->KeyPress(KEY_KP_ENTER))
						ShapeGhostCommit();
					else if(Input()->KeyPress(KEY_ESCAPE))
						ShapeGhostClear();
				}

				// Fade tool: click on handle / node to drag, on segment to insert, on empty to append
				if(m_FadeTool && !m_ShowPicker && Ui()->MouseButton(0) && s_Operation == OP_NONE && !Input()->ModifierIsPressed() && !Map()->m_QuadKnife.m_Active)
				{
					int DragKind = FADE_DRAG_NONE;
					int Handle = FadeToolHandleHitTest(wx, wy, DragKind);
					if(Handle < 0)
					{
						Handle = FadeToolHitTest(wx, wy);
						if(Handle >= 0)
							DragKind = FADE_DRAG_NODE;
					}
					if(Handle < 0)
					{
						vec2 Closest;
						const int SegHover = FadeToolFindHoveredSegment(wx, wy, Closest);
						if(SegHover >= 0)
						{
							m_vFadeNodes.insert(m_vFadeNodes.begin() + SegHover + 1, Closest);
							m_vFadeHandleIn.insert(m_vFadeHandleIn.begin() + SegHover + 1, vec2(0.0f, 0.0f));
							m_vFadeHandleOut.insert(m_vFadeHandleOut.begin() + SegHover + 1, vec2(0.0f, 0.0f));
							Handle = SegHover + 1;
						}
						else
						{
							m_vFadeNodes.push_back(vec2(wx, wy));
							m_vFadeHandleIn.push_back(vec2(0.0f, 0.0f));
							m_vFadeHandleOut.push_back(vec2(0.0f, 0.0f));
							Handle = (int)m_vFadeNodes.size() - 1;
						}
						// Recompute auto-handles for the new node and its neighbors.
						const int N = (int)m_vFadeNodes.size();
						for(int Off = -1; Off <= 1; ++Off)
						{
							const int Idx = Handle + Off;
							if(Idx >= 0 && Idx < N)
								FadeRecomputeAutoHandles(Idx);
						}
						DragKind = FADE_DRAG_NODE;
					}
					if(Handle >= 0 && Handle < (int)m_vFadeNodes.size())
					{
						Ui()->SetActiveItem(&m_MapEditorId);
						m_FadeActiveHandle = Handle;
						m_FadeDragKind = DragKind;
						m_FadeDragStartMouse = vec2(wx, wy);
						m_FadeDragStartNodePos = m_vFadeNodes[Handle];
						if(Handle < (int)m_vFadeHandleIn.size())
							m_FadeDragStartHandleIn = m_vFadeHandleIn[Handle];
						if(Handle < (int)m_vFadeHandleOut.size())
							m_FadeDragStartHandleOut = m_vFadeHandleOut[Handle];
						s_Operation = OP_FADE_DRAG;
					}
				}

				// 3D tool: click on a node to start dragging it
				if(m_ThreeDTool && m_ThreeDInit && !m_ShowPicker && Ui()->MouseButton(0) && s_Operation == OP_NONE && !Input()->ModifierIsPressed() && !Map()->m_QuadKnife.m_Active)
				{
					int Node = ThreeDToolHitNode(wx, wy);
					if(Node >= 0)
					{
						Ui()->SetActiveItem(&m_MapEditorId);
						m_ThreeDDragNode = Node;
						m_ThreeDDragStartMouse = vec2(wx, wy);
						m_ThreeDDragStartCenter = m_ThreeDCenter;
						m_ThreeDDragStartScaleX = m_ThreeDScaleX;
						m_ThreeDDragStartScaleY = m_ThreeDScaleY;
						m_ThreeDDragStartScaleZ = m_ThreeDScaleZ;
						m_ThreeDDragStartRx = m_ThreeDRxDeg;
						m_ThreeDDragStartRy = m_ThreeDRyDeg;
						s_Operation = OP_THREED_DRAG;
					}
				}

				// Shape ghost: click on a node to drag it
				if(m_ShapeTool && m_ShapeGhostInit && !m_ShowPicker && Ui()->MouseButton(0) && s_Operation == OP_NONE && !Input()->ModifierIsPressed() && !Map()->m_QuadKnife.m_Active)
				{
					Ui()->SetActiveItem(&m_MapEditorId);
					int Node = ShapeGhostHitNode(wx, wy);
					if(Node >= 0)
					{
						m_ShapeGhostDragNode = Node;
						m_ShapeGhostDragStartMouse = vec2(wx, wy);
						m_ShapeGhostDragStartX1 = m_ShapeGhostX1;
						m_ShapeGhostDragStartY1 = m_ShapeGhostY1;
						m_ShapeGhostDragStartX2 = m_ShapeGhostX2;
						m_ShapeGhostDragStartY2 = m_ShapeGhostY2;
						s_Operation = OP_SHAPE_GHOST_DRAG;
					}
				}

				// Symmetry axis drag — narrow hit area so it doesn't clobber tool clicks
				if(m_SymmetryMode != SYM_NONE && Ui()->MouseButton(0) && s_Operation == OP_NONE && !Input()->ModifierIsPressed() && !Map()->m_QuadKnife.m_Active)
				{
					const float HitRange = 6.0f;
					const bool ModeH = (m_SymmetryMode == SYM_H || m_SymmetryMode == SYM_BOTH);
					const bool ModeV = (m_SymmetryMode == SYM_V || m_SymmetryMode == SYM_BOTH);
					const float DistX = std::abs(wx - m_SymmetryCenter.x);
					const float DistY = std::abs(wy - m_SymmetryCenter.y);
					if(m_SymmetryMode == SYM_BOTH && DistX < HitRange && DistY < HitRange)
					{
						Ui()->SetActiveItem(&m_MapEditorId);
						s_Operation = OP_SYM_DRAG_CENTER;
					}
					else if(ModeH && DistX < HitRange)
					{
						Ui()->SetActiveItem(&m_MapEditorId);
						s_Operation = OP_SYM_DRAG_H;
					}
					else if(ModeV && DistY < HitRange)
					{
						Ui()->SetActiveItem(&m_MapEditorId);
						s_Operation = OP_SYM_DRAG_V;
					}
				}

				if(!Input()->ModifierIsPressed() && Ui()->MouseButton(0) && s_Operation == OP_NONE && !Map()->m_QuadKnife.m_Active && !m_ThreeDTool)
				{
					Ui()->SetActiveItem(&m_MapEditorId);

					bool DoFill = m_FillTool && !m_pBrush->IsEmpty();
					if(DoFill)
					{
						for(const auto &pLayer : m_pBrush->m_vpLayers)
						{
							if(pLayer->m_Type != LAYERTYPE_TILES)
							{
								DoFill = false;
								break;
							}
						}
					}

					bool DoShape = m_ShapeTool && !m_pBrush->IsEmpty();
					if(DoShape)
					{
						for(const auto &pLayer : m_pBrush->m_vpLayers)
						{
							if(pLayer->m_Type != LAYERTYPE_TILES)
							{
								DoShape = false;
								break;
							}
							const auto pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
							if(pTiles->m_Width != 1 || pTiles->m_Height != 1)
							{
								DoShape = false;
								break;
							}
						}
					}

					bool DoLine = m_LineTool && !m_ShowPicker;
					if(DoLine)
					{
						for(const auto &pLayer : m_pBrush->m_vpLayers)
						{
							if(pLayer->m_Type != LAYERTYPE_TILES)
							{
								DoLine = false;
								break;
							}
							const auto pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
							if(pTiles->m_Width != 1 || pTiles->m_Height != 1)
							{
								DoLine = false;
								break;
							}
						}
					}

					if(DoLine)
					{
						const int N = (int)m_vLineNodes.size();
						int DragKind = LINE_DRAG_NONE;
						int Hover = LineToolHandleHitTest(wx, wy, DragKind);
						if(Hover < 0)
						{
							Hover = LineToolFindHoveredNode(wx, wy);
							if(Hover >= 0)
								DragKind = LINE_DRAG_NODE;
						}

						// Resolve overlap of first/last node: prefer last so the user can drag it.
						if(Hover == 0 && N >= 3 && DragKind == LINE_DRAG_NODE)
						{
							const vec2 First = m_vLineNodes[0];
							const vec2 Last = m_vLineNodes[N - 1];
							const float DistFirstSq = (wx - First.x) * (wx - First.x) + (wy - First.y) * (wy - First.y);
							const float DistLastSq = (wx - Last.x) * (wx - Last.x) + (wy - Last.y) * (wy - Last.y);
							if(DistFirstSq >= DistLastSq)
								Hover = N - 1;
						}

						if(Hover >= 0)
						{
							m_LineActiveNode = Hover;
							m_LineDragKind = DragKind;
							m_LineClickedExisting = true;
							// Selecting a node = its handles become visible/interactive.
							// Handle drags don't change selection (they belong to the already-selected node).
							if(DragKind == LINE_DRAG_NODE)
								m_LineSelectedNode = Hover;
						}
						else
						{
							m_LineClickedExisting = false;
							vec2 Closest;
							int SegHover = LineToolFindHoveredSegment(wx, wy, Closest);
							int NewIdx;
							if(SegHover >= 0)
							{
								m_vLineNodes.insert(m_vLineNodes.begin() + SegHover + 1, Closest);
								m_vLineNodeKinds.insert(m_vLineNodeKinds.begin() + SegHover + 1, m_LineDefaultNodeKind);
								m_vLineHandleIn.insert(m_vLineHandleIn.begin() + SegHover + 1, vec2(0.0f, 0.0f));
								m_vLineHandleOut.insert(m_vLineHandleOut.begin() + SegHover + 1, vec2(0.0f, 0.0f));
								NewIdx = SegHover + 1;
								// Inserting bumps indices >= NewIdx by one.
								if(m_LineSelectedNode >= NewIdx)
									++m_LineSelectedNode;
							}
							else
							{
								m_vLineNodes.emplace_back(wx, wy);
								m_vLineNodeKinds.push_back(m_LineDefaultNodeKind);
								m_vLineHandleIn.push_back(vec2(0.0f, 0.0f));
								m_vLineHandleOut.push_back(vec2(0.0f, 0.0f));
								NewIdx = (int)m_vLineNodes.size() - 1;
							}
							const int N2 = (int)m_vLineNodes.size();
							for(int Off = -1; Off <= 1; ++Off)
							{
								const int Idx = NewIdx + Off;
								if(Idx >= 0 && Idx < N2)
									LineRecomputeAutoHandles(Idx);
							}
							m_LineActiveNode = NewIdx;
							m_LineDragKind = LINE_DRAG_NODE;
							m_LineSelectedNode = NewIdx;
						}
						m_LineDragStartMouse = vec2(wx, wy);
						m_LineDragMoved = false;
						if(m_LineActiveNode >= 0 && m_LineActiveNode < (int)m_vLineHandleIn.size())
							m_LineDragStartHandleIn = m_vLineHandleIn[m_LineActiveNode];
						if(m_LineActiveNode >= 0 && m_LineActiveNode < (int)m_vLineHandleOut.size())
							m_LineDragStartHandleOut = m_vLineHandleOut[m_LineActiveNode];
						s_Operation = OP_LINE_DRAG;
					}
					else if(m_SelectTool && m_SelectFreehand && !m_ShowPicker)
					{
						s_Operation = OP_SELECT_FREEHAND;
						m_vSelectPath.clear();
						m_vSelectPath.emplace_back(wx, wy);
					}
					else if(m_pBrush->IsEmpty() || (m_SelectTool && !m_SelectFreehand))
					{
						m_pBrush->Clear();
						SelectToolCancelOrig();
						s_Operation = OP_BRUSH_GRAB;
					}
					else if(DoShape)
					{
						ShapeGhostClear();
						s_Operation = OP_SHAPE_DRAW;
					}
					else if(DoFill)
					{
						s_Operation = OP_BRUSH_FILL;
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k;
							if(m_pBrush->m_vpLayers.size() != NumEditLayers)
								BrushIndex = 0;

							if(apEditLayers[k].second->m_Type != LAYERTYPE_TILES)
								continue;
							if(m_pBrush->m_vpLayers[BrushIndex]->m_Type != LAYERTYPE_TILES)
								continue;

							std::shared_ptr<CLayerTiles> pLayer = std::static_pointer_cast<CLayerTiles>(apEditLayers[k].second);
							std::shared_ptr<CLayerTiles> pBrushLayer = std::static_pointer_cast<CLayerTiles>(m_pBrush->m_vpLayers[BrushIndex]);

							if((!pLayer->m_HasTele || pBrushLayer->m_HasTele) && (!pLayer->m_HasSpeedup || pBrushLayer->m_HasSpeedup) && (!pLayer->m_HasFront || pBrushLayer->m_HasFront) && (!pLayer->m_HasGame || pBrushLayer->m_HasGame) && (!pLayer->m_HasSwitch || pBrushLayer->m_HasSwitch) && (!pLayer->m_HasTune || pBrushLayer->m_HasTune))
								pLayer->BrushFill(pBrushLayer.get(), vec2(wx, wy));
						}
					}
					else
					{
						s_Operation = OP_BRUSH_DRAW;
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k;
							if(m_pBrush->m_vpLayers.size() != NumEditLayers)
								BrushIndex = 0;

							if(apEditLayers[k].second->m_Type == m_pBrush->m_vpLayers[BrushIndex]->m_Type)
								apEditLayers[k].second->BrushPlace(m_pBrush->m_vpLayers[BrushIndex].get(), vec2(wx, wy));
						}
					}

					std::shared_ptr<CLayerTiles> pLayer = std::static_pointer_cast<CLayerTiles>(Map()->SelectedLayerType(0, LAYERTYPE_TILES));
					if(Input()->ShiftIsPressed() && pLayer)
						s_Operation = OP_BRUSH_PAINT;
				}

				if(!m_pBrush->IsEmpty())
				{
					m_pBrush->m_OffsetX = -(int)wx;
					m_pBrush->m_OffsetY = -(int)wy;
					for(const auto &pLayer : m_pBrush->m_vpLayers)
					{
						if(pLayer->m_Type == LAYERTYPE_TILES)
						{
							m_pBrush->m_OffsetX = -(int)(wx / 32.0f) * 32;
							m_pBrush->m_OffsetY = -(int)(wy / 32.0f) * 32;
							break;
						}
					}

					std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
					if(!m_ShowPicker && pGroup)
					{
						m_pBrush->m_OffsetX += pGroup->m_OffsetX;
						m_pBrush->m_OffsetY += pGroup->m_OffsetY;
						m_pBrush->m_ParallaxX = pGroup->m_ParallaxX;
						m_pBrush->m_ParallaxY = pGroup->m_ParallaxY;
						m_pBrush->Render();

						CUIRect BorderRect;
						BorderRect.x = 0.0f;
						BorderRect.y = 0.0f;
						m_pBrush->GetSize(&BorderRect.w, &BorderRect.h);
						BorderRect.DrawOutline(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
					}
				}
			}
		}

		// quad & sound editing
		{
			if(!m_ShowPicker && m_pBrush->IsEmpty())
			{
				// fetch layers
				std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
				if(pGroup)
					pGroup->MapScreen();

				for(size_t k = 0; k < NumEditLayers; k++)
				{
					auto &[LayerIndex, pEditLayer] = apEditLayers[k];

					if(pEditLayer->m_Type == LAYERTYPE_QUADS)
					{
						std::shared_ptr<CLayerQuads> pLayer = std::static_pointer_cast<CLayerQuads>(pEditLayer);

						if(m_ActiveEnvelopePreview == EEnvelopePreview::NONE)
							m_ActiveEnvelopePreview = EEnvelopePreview::ALL;

						if(Map()->m_QuadKnife.m_Active)
						{
							DoQuadKnife(Map()->m_vSelectedQuads[Map()->m_QuadKnife.m_SelectedQuadIndex]);
						}
						else
						{
							UpdateHotQuadPoint(pLayer.get());

							Graphics()->TextureClear();
							Graphics()->QuadsBegin();
							for(size_t i = 0; i < pLayer->m_vQuads.size(); i++)
							{
								for(int v = 0; v < 4; v++)
									DoQuadPoint(LayerIndex, pLayer, &pLayer->m_vQuads[i], i, v);

								DoQuad(LayerIndex, pLayer, &pLayer->m_vQuads[i], i);
							}
							Graphics()->QuadsEnd();
						}
					}
					else if(pEditLayer->m_Type == LAYERTYPE_SOUNDS)
					{
						std::shared_ptr<CLayerSounds> pLayer = std::static_pointer_cast<CLayerSounds>(pEditLayer);

						UpdateHotSoundSource(pLayer.get());

						Graphics()->TextureClear();
						Graphics()->QuadsBegin();
						for(size_t i = 0; i < pLayer->m_vSources.size(); i++)
						{
							DoSoundSource(LayerIndex, &pLayer->m_vSources[i], i);
						}
						Graphics()->QuadsEnd();
					}
				}

				Ui()->MapScreen();
			}
		}

		// menu proof selection
		if(MapView()->ProofMode()->IsModeMenu() && !m_ShowPicker)
		{
			MapView()->ProofMode()->InitMenuBackgroundPositions();
			const std::vector<vec2> &MenuBackgroundPositions = MapView()->ProofMode()->MenuBackgroundPositions();
			for(int i = 0; i < (int)MenuBackgroundPositions.size(); i++)
			{
				vec2 Pos = MenuBackgroundPositions[i];
				const void *pId = &MenuBackgroundPositions[i];
				Pos += MapView()->GetWorldOffset() - MenuBackgroundPositions[MapView()->ProofMode()->CurrentMenuProofIndex()];
				Pos.y -= 3.0f;

				if(distance(Pos, m_MouseWorldNoParaPos) <= 20.0f)
				{
					Ui()->SetHotItem(pId);

					if(i != MapView()->ProofMode()->CurrentMenuProofIndex() && Ui()->CheckActiveItem(pId))
					{
						if(!Ui()->MouseButton(0))
						{
							MapView()->ProofMode()->SetCurrentMenuProofIndex(i);
							MapView()->SetWorldOffset(MenuBackgroundPositions[i]);
							Ui()->SetActiveItem(nullptr);
						}
					}
					else if(Ui()->HotItem() == pId)
					{
						char aTooltipPrefix[32] = "Switch proof position to";
						if(i == MapView()->ProofMode()->CurrentMenuProofIndex())
							str_copy(aTooltipPrefix, "Current proof position at");

						char aNumBuf[8];
						if(i < (TILE_TIME_CHECKPOINT_LAST - TILE_TIME_CHECKPOINT_FIRST))
							str_format(aNumBuf, sizeof(aNumBuf), "#%d", i + 1);
						else
							aNumBuf[0] = '\0';

						char aTooltipPositions[128];
						str_format(aTooltipPositions, sizeof(aTooltipPositions), "%s %s", MapView()->ProofMode()->MenuBackgroundPositionName(i), aNumBuf);

						for(int k : MapView()->ProofMode()->MenuBackgroundCollisions(i))
						{
							if(k == MapView()->ProofMode()->CurrentMenuProofIndex())
								str_copy(aTooltipPrefix, "Current proof position at");

							Pos = MenuBackgroundPositions[k];
							Pos += MapView()->GetWorldOffset() - MenuBackgroundPositions[MapView()->ProofMode()->CurrentMenuProofIndex()];
							Pos.y -= 3.0f;

							if(distance(Pos, m_MouseWorldNoParaPos) > 20.0f)
								continue;

							if(i < (TILE_TIME_CHECKPOINT_LAST - TILE_TIME_CHECKPOINT_FIRST))
								str_format(aNumBuf, sizeof(aNumBuf), "#%d", k + 1);
							else
								aNumBuf[0] = '\0';

							char aTooltipPositionsCopy[128];
							str_copy(aTooltipPositionsCopy, aTooltipPositions);
							str_format(aTooltipPositions, sizeof(aTooltipPositions), "%s, %s %s", aTooltipPositionsCopy, MapView()->ProofMode()->MenuBackgroundPositionName(k), aNumBuf);
						}
						str_format(m_aTooltip, sizeof(m_aTooltip), "%s %s.", aTooltipPrefix, aTooltipPositions);

						if(Ui()->MouseButton(0))
							Ui()->SetActiveItem(pId);
					}
					break;
				}
			}
		}

		if(!Input()->ModifierIsPressed() && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
		{
			float PanSpeed = Input()->ShiftIsPressed() ? 200.0f : 64.0f;
			if(Input()->KeyPress(KEY_A))
				MapView()->OffsetWorld({-PanSpeed * m_MouseWorldScale, 0});
			else if(Input()->KeyPress(KEY_D))
				MapView()->OffsetWorld({PanSpeed * m_MouseWorldScale, 0});
			if(Input()->KeyPress(KEY_W))
				MapView()->OffsetWorld({0, -PanSpeed * m_MouseWorldScale});
			else if(Input()->KeyPress(KEY_S))
				MapView()->OffsetWorld({0, PanSpeed * m_MouseWorldScale});

			// Tool hotkeys: F=Fill, B=Shape, L=Line (mutually exclusive)
			if(!Input()->ShiftIsPressed() && !Ui()->IsPopupOpen())
			{
				if(Input()->KeyPress(KEY_F))
				{
					m_FillTool = !m_FillTool;
					if(m_FillTool)
					{
						m_ShapeTool = false;
						if(m_LineTool)
							LineToolCancel();
						m_LineTool = false;
						m_SelectTool = false;
						SelectToolCancelOrig();
						ThreeDToolCancel();
						m_ThreeDTool = false;
					}
				}
				else if(Input()->KeyPress(KEY_B))
				{
					m_ShapeTool = !m_ShapeTool;
					if(m_ShapeTool)
					{
						m_FillTool = false;
						if(m_LineTool)
							LineToolCancel();
						m_LineTool = false;
						m_SelectTool = false;
						SelectToolCancelOrig();
						ThreeDToolCancel();
						m_ThreeDTool = false;
					}
					else
						ShapeGhostClear();
				}
				else if(Input()->KeyPress(KEY_L))
				{
					if(m_LineTool)
						LineToolCancel();
					m_LineTool = !m_LineTool;
					if(m_LineTool)
					{
						m_FillTool = false;
						m_ShapeTool = false;
						if(m_FadeTool)
							FadeToolCancel();
						m_FadeTool = false;
						m_SelectTool = false;
						SelectToolCancelOrig();
						ThreeDToolCancel();
						m_ThreeDTool = false;
					}
				}
				else if(Input()->KeyPress(KEY_G) && !Input()->ShiftIsPressed())
				{
					if(m_FadeTool)
						FadeToolCancel();
					m_FadeTool = !m_FadeTool;
					if(m_FadeTool)
					{
						m_FillTool = false;
						m_ShapeTool = false;
						if(m_LineTool)
							LineToolCancel();
						m_LineTool = false;
						m_SelectTool = false;
						SelectToolCancelOrig();
						ThreeDToolCancel();
						m_ThreeDTool = false;
						FadeToolEnsureInit();
					}
				}
			}
		}
	}

	if(Ui()->CheckActiveItem(&m_MapEditorId) && m_pContainerPanned == nullptr)
	{
		// release mouse
		if(!Ui()->MouseButton(0))
		{
			if(s_Operation == OP_BRUSH_DRAW || s_Operation == OP_BRUSH_FILL || s_Operation == OP_SHAPE_DRAW)
			{
				std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);

				if(!pAction->IsEmpty()) // Avoid recording tile draw action when placing quads only
					Map()->m_EditorHistory.RecordAction(pAction);
			}
			if(s_Operation == OP_LINE_DRAG)
			{
				// Click-without-drag on the FIRST node (when N≥3 and the loop isn't already
				// closed): close the loop. One-way — clicking first when closed is a no-op so
				// the closing segment can't disappear unexpectedly. To reopen, restart with Esc
				// or right-click-delete a node until N<3 (auto-opens).
				// Kind-cycling moved to the C key.
				if(!m_LineDragMoved && m_LineClickedExisting && m_LineDragKind == LINE_DRAG_NODE && m_LineActiveNode == 0)
				{
					const int N = (int)m_vLineNodes.size();
					if(N >= 3 && !m_LineClosed)
						m_LineClosed = true;
				}
				m_LineActiveNode = -1;
				m_LineDragKind = LINE_DRAG_NONE;
				m_LineDragMoved = false;
				m_LineClickedExisting = false;
			}
			if(s_Operation == OP_FADE_DRAG)
			{
				m_FadeActiveHandle = -1;
				m_FadeDragKind = FADE_DRAG_NONE;
			}
			if(s_Operation == OP_THREED_DRAG)
			{
				m_ThreeDDragNode = -1;
			}
			if(s_Operation == OP_SHAPE_GHOST_DRAG)
			{
				m_ShapeGhostDragNode = -1;
			}

			s_Operation = OP_NONE;
			Ui()->SetActiveItem(nullptr);
		}
	}

	if(!m_ShowPicker && Map()->SelectedGroup() && Map()->SelectedGroup()->m_UseClipping)
	{
		std::shared_ptr<CLayerGroup> pGameGroup = Map()->m_pGameGroup;
		pGameGroup->MapScreen();

		CUIRect ClipRect;
		ClipRect.x = Map()->SelectedGroup()->m_ClipX;
		ClipRect.y = Map()->SelectedGroup()->m_ClipY;
		ClipRect.w = Map()->SelectedGroup()->m_ClipW;
		ClipRect.h = Map()->SelectedGroup()->m_ClipH;
		ClipRect.DrawOutline(ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f));
	}

	if(!m_ShowPicker)
		MapView()->ProofMode()->RenderScreenSizes();

	if(!m_ShowPicker && m_ShowEnvelopePreview && m_ActiveEnvelopePreview != EEnvelopePreview::NONE)
	{
		const std::shared_ptr<CLayer> pSelectedLayer = Map()->SelectedLayer(0);
		if(pSelectedLayer != nullptr && pSelectedLayer->m_Type == LAYERTYPE_QUADS)
		{
			DoQuadEnvelopes(static_cast<const CLayerQuads *>(pSelectedLayer.get()));
		}
		m_ActiveEnvelopePreview = EEnvelopePreview::NONE;
	}

	// Tool ghost rendering — always visible (incl. when cursor is on toolbar) so the user sees
	// what they're shaping. Hidden only during canvas pan, since the user is moving the view
	// and doesn't want the preview to follow.
	{
		const bool IsPanning = (s_Operation == OP_PAN_WORLD || s_Operation == OP_PAN_EDITOR);
		std::shared_ptr<CLayerGroup> pCurrentGroup = Map()->SelectedGroup();
		if(pCurrentGroup)
			pCurrentGroup->MapScreen();
		if(!IsPanning && !m_ShowPicker && m_LineTool)
			LineToolRenderAndInteract(wx, wy);
		if(!IsPanning && !m_ShowPicker && m_FadeTool)
			FadeToolRender(wx, wy);
		if(!IsPanning && !m_ShowPicker && m_ThreeDTool)
			ThreeDToolRenderAndInteract(wx, wy, mx, my);
		if(!IsPanning && !m_ShowPicker && m_ShapeTool && m_ShapeGhostInit)
			ShapeGhostRenderAndInteract(wx, wy, mx, my);
		if(m_SymmetryMode != SYM_NONE)
		{
			const float ViewW = MapView()->ScaleLength(4096.0f);
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(0.3f, 1.0f, 0.5f, 0.8f);
			if(m_SymmetryMode == SYM_H || m_SymmetryMode == SYM_BOTH)
			{
				IGraphics::CLineItem L(m_SymmetryCenter.x, m_SymmetryCenter.y - ViewW, m_SymmetryCenter.x, m_SymmetryCenter.y + ViewW);
				Graphics()->LinesDraw(&L, 1);
			}
			if(m_SymmetryMode == SYM_V || m_SymmetryMode == SYM_BOTH)
			{
				IGraphics::CLineItem L(m_SymmetryCenter.x - ViewW, m_SymmetryCenter.y, m_SymmetryCenter.x + ViewW, m_SymmetryCenter.y);
				Graphics()->LinesDraw(&L, 1);
			}
			Graphics()->LinesEnd();
		}
	}

	Ui()->MapScreen();
}

void CEditor::UpdateHotQuadPoint(const CLayerQuads *pLayer)
{
	const vec2 MouseWorld = Ui()->MouseWorldPos();

	float MinDist = 500.0f;
	const void *pMinPointId = nullptr;

	const auto UpdateMinimum = [&](vec2 Position, const void *pId) {
		const float CurrDist = length_squared((Position - MouseWorld) / m_MouseWorldScale);
		if(CurrDist < MinDist)
		{
			MinDist = CurrDist;
			pMinPointId = pId;
			return true;
		}
		return false;
	};

	for(const CQuad &Quad : pLayer->m_vQuads)
	{
		if(m_ShowEnvelopePreview &&
			m_ActiveEnvelopePreview != EEnvelopePreview::NONE &&
			Quad.m_PosEnv >= 0 &&
			Quad.m_PosEnv < (int)Map()->m_vpEnvelopes.size())
		{
			for(const auto &EnvPoint : Map()->m_vpEnvelopes[Quad.m_PosEnv]->m_vPoints)
			{
				const vec2 Position = vec2(fx2f(Quad.m_aPoints[4].x) + fx2f(EnvPoint.m_aValues[0]), fx2f(Quad.m_aPoints[4].y) + fx2f(EnvPoint.m_aValues[1]));
				if(UpdateMinimum(Position, &EnvPoint) && Ui()->ActiveItem() == nullptr)
				{
					Map()->m_CurrentQuadIndex = &Quad - pLayer->m_vQuads.data();
				}
			}
		}

		for(const auto &Point : Quad.m_aPoints)
		{
			UpdateMinimum(vec2(fx2f(Point.x), fx2f(Point.y)), &Point);
		}
	}

	if(pMinPointId != nullptr)
	{
		Ui()->SetHotItem(pMinPointId);
	}
}

void CEditor::DoColorPickerButton(const void *pId, const CUIRect *pRect, ColorRGBA Color, const std::function<void(ColorRGBA Color)> &SetColor)
{
	CUIRect ColorRect;
	pRect->Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f * Ui()->ButtonColorMul(pId)), IGraphics::CORNER_ALL, 3.0f);
	pRect->Margin(1.0f, &ColorRect);
	ColorRect.Draw(Color, IGraphics::CORNER_ALL, 3.0f);

	const int ButtonResult = DoButtonLogic(pId, 0, pRect, BUTTONFLAG_ALL, "Click to show the color picker. Shift+right click to copy color to clipboard. Shift+left click to paste color from clipboard.");
	if(Input()->ShiftIsPressed())
	{
		if(ButtonResult == 1)
		{
			std::string Clipboard = Input()->GetClipboardText();
			if(Clipboard[0] == '#' || Clipboard[0] == '$') // ignore leading # (web color format) and $ (console color format)
				Clipboard = Clipboard.substr(1);
			if(str_isallnum_hex(Clipboard.c_str()))
			{
				std::optional<ColorRGBA> ParsedColor = color_parse<ColorRGBA>(Clipboard.c_str());
				if(ParsedColor)
				{
					m_ColorPickerPopupContext.m_State = EEditState::ONE_GO;
					SetColor(ParsedColor.value());
				}
			}
		}
		else if(ButtonResult == 2)
		{
			char aClipboard[9];
			str_format(aClipboard, sizeof(aClipboard), "%08X", Color.PackAlphaLast());
			Input()->SetClipboardText(aClipboard);
		}
	}
	else if(ButtonResult > 0)
	{
		if(m_ColorPickerPopupContext.m_ColorMode == CUi::SColorPickerPopupContext::MODE_UNSET)
			m_ColorPickerPopupContext.m_ColorMode = CUi::SColorPickerPopupContext::MODE_RGBA;
		m_ColorPickerPopupContext.m_RgbaColor = Color;
		m_ColorPickerPopupContext.m_HslaColor = color_cast<ColorHSLA>(Color);
		m_ColorPickerPopupContext.m_HsvaColor = color_cast<ColorHSVA>(m_ColorPickerPopupContext.m_HslaColor);
		m_ColorPickerPopupContext.m_Alpha = true;
		m_pColorPickerPopupActiveId = pId;
		Ui()->ShowPopupColorPicker(Ui()->MouseX(), Ui()->MouseY(), &m_ColorPickerPopupContext);
	}

	if(Ui()->IsPopupOpen(&m_ColorPickerPopupContext))
	{
		if(m_pColorPickerPopupActiveId == pId)
			SetColor(m_ColorPickerPopupContext.m_RgbaColor);
	}
	else
	{
		m_pColorPickerPopupActiveId = nullptr;
		if(m_ColorPickerPopupContext.m_State == EEditState::EDITING)
		{
			m_ColorPickerPopupContext.m_State = EEditState::END;
			SetColor(m_ColorPickerPopupContext.m_RgbaColor);
			m_ColorPickerPopupContext.m_State = EEditState::NONE;
		}
	}
}

bool CEditor::IsAllowPlaceUnusedTiles() const
{
	// explicit allow and implicit allow
	return m_AllowPlaceUnusedTiles != EUnusedEntities::NOT_ALLOWED;
}

void CEditor::RenderLayers(CUIRect LayersBox)
{
	const float RowHeight = 12.0f;
	char aBuf[64];

	CUIRect UnscrolledLayersBox = LayersBox;

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollbarWidth = 10.0f;
	ScrollParams.m_ScrollbarMargin = 3.0f;
	ScrollParams.m_ScrollUnit = RowHeight * 5.0f;
	s_ScrollRegion.Begin(&LayersBox, &ScrollOffset, &ScrollParams);
	LayersBox.y += ScrollOffset.y;

	enum
	{
		OP_NONE = 0,
		OP_CLICK,
		OP_LAYER_DRAG,
		OP_GROUP_DRAG
	};
	static int s_Operation = OP_NONE;
	static int s_PreviousOperation = OP_NONE;
	static const void *s_pDraggedButton = nullptr;
	static float s_InitialMouseY = 0;
	static float s_InitialCutHeight = 0;
	constexpr float MinDragDistance = 5.0f;
	int GroupAfterDraggedLayer = -1;
	int LayerAfterDraggedLayer = -1;
	bool DraggedPositionFound = false;
	bool MoveLayers = false;
	bool MoveGroup = false;
	bool StartDragLayer = false;
	bool StartDragGroup = false;
	std::vector<int> vButtonsPerGroup;

	auto SetOperation = [](int Operation) {
		if(Operation != s_Operation)
		{
			s_PreviousOperation = s_Operation;
			s_Operation = Operation;
			if(Operation == OP_NONE)
			{
				s_pDraggedButton = nullptr;
			}
		}
	};

	vButtonsPerGroup.reserve(Map()->m_vpGroups.size());
	for(const std::shared_ptr<CLayerGroup> &pGroup : Map()->m_vpGroups)
	{
		vButtonsPerGroup.push_back(pGroup->m_vpLayers.size() + 1);
	}

	if(s_pDraggedButton != nullptr && Ui()->ActiveItem() != s_pDraggedButton)
	{
		SetOperation(OP_NONE);
	}

	if(s_Operation == OP_LAYER_DRAG || s_Operation == OP_GROUP_DRAG)
	{
		float MinDraggableValue = UnscrolledLayersBox.y;
		float MaxDraggableValue = MinDraggableValue;
		for(int NumButtons : vButtonsPerGroup)
		{
			MaxDraggableValue += NumButtons * (RowHeight + 2.0f) + 5.0f;
		}
		MaxDraggableValue += ScrollOffset.y;

		if(s_Operation == OP_GROUP_DRAG)
		{
			MaxDraggableValue -= vButtonsPerGroup[Map()->m_SelectedGroup] * (RowHeight + 2.0f) + 5.0f;
		}
		else if(s_Operation == OP_LAYER_DRAG)
		{
			MinDraggableValue += RowHeight + 2.0f;
			MaxDraggableValue -= Map()->m_vSelectedLayers.size() * (RowHeight + 2.0f) + 5.0f;
		}

		UnscrolledLayersBox.HSplitTop(s_InitialCutHeight, nullptr, &UnscrolledLayersBox);
		UnscrolledLayersBox.y -= s_InitialMouseY - Ui()->MouseY();

		UnscrolledLayersBox.y = std::clamp(UnscrolledLayersBox.y, MinDraggableValue, MaxDraggableValue);

		UnscrolledLayersBox.w = LayersBox.w;
	}

	static bool s_ScrollToSelectionNext = false;
	const bool ScrollToSelection = LayerSelector()->SelectByTile() || s_ScrollToSelectionNext;
	s_ScrollToSelectionNext = false;

	// render layers
	for(int g = 0; g < (int)Map()->m_vpGroups.size(); g++)
	{
		if(s_Operation == OP_LAYER_DRAG && g > 0 && !DraggedPositionFound && Ui()->MouseY() < LayersBox.y + RowHeight / 2)
		{
			DraggedPositionFound = true;
			GroupAfterDraggedLayer = g;

			LayerAfterDraggedLayer = Map()->m_vpGroups[g - 1]->m_vpLayers.size();

			CUIRect Slot;
			LayersBox.HSplitTop(Map()->m_vSelectedLayers.size() * (RowHeight + 2.0f), &Slot, &LayersBox);
			s_ScrollRegion.AddRect(Slot);
		}

		CUIRect Slot, VisibleToggle;
		if(s_Operation == OP_GROUP_DRAG)
		{
			if(g == Map()->m_SelectedGroup)
			{
				UnscrolledLayersBox.HSplitTop(RowHeight, &Slot, &UnscrolledLayersBox);
				UnscrolledLayersBox.HSplitTop(2.0f, nullptr, &UnscrolledLayersBox);
			}
			else if(!DraggedPositionFound && Ui()->MouseY() < LayersBox.y + RowHeight * vButtonsPerGroup[g] / 2 + 3.0f)
			{
				DraggedPositionFound = true;
				GroupAfterDraggedLayer = g;

				CUIRect TmpSlot;
				if(Map()->m_vpGroups[Map()->m_SelectedGroup]->m_Collapse)
					LayersBox.HSplitTop(RowHeight + 7.0f, &TmpSlot, &LayersBox);
				else
					LayersBox.HSplitTop(vButtonsPerGroup[Map()->m_SelectedGroup] * (RowHeight + 2.0f) + 5.0f, &TmpSlot, &LayersBox);
				s_ScrollRegion.AddRect(TmpSlot, false);
			}
		}
		if(s_Operation != OP_GROUP_DRAG || g != Map()->m_SelectedGroup)
		{
			LayersBox.HSplitTop(RowHeight, &Slot, &LayersBox);

			CUIRect TmpRect;
			LayersBox.HSplitTop(2.0f, &TmpRect, &LayersBox);
			s_ScrollRegion.AddRect(TmpRect);
		}

		if(s_ScrollRegion.AddRect(Slot))
		{
			Slot.VSplitLeft(15.0f, &VisibleToggle, &Slot);

			const int MouseClick = DoButton_FontIcon(&Map()->m_vpGroups[g]->m_Visible, Map()->m_vpGroups[g]->m_Visible ? FontIcon::EYE : FontIcon::EYE_SLASH, Map()->m_vpGroups[g]->m_Collapse ? 1 : 0, &VisibleToggle, BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT, "Left click to toggle visibility. Right click to show this group only.", IGraphics::CORNER_L, 8.0f);
			if(MouseClick == 1)
			{
				Map()->m_vpGroups[g]->m_Visible = !Map()->m_vpGroups[g]->m_Visible;
			}
			else if(MouseClick == 2)
			{
				if(Input()->ShiftIsPressed())
				{
					if(g != Map()->m_SelectedGroup)
						Map()->SelectLayer(0, g);
				}

				int NumActive = 0;
				for(auto &Group : Map()->m_vpGroups)
				{
					if(Group == Map()->m_vpGroups[g])
					{
						Group->m_Visible = true;
						continue;
					}

					if(Group->m_Visible)
					{
						Group->m_Visible = false;
						NumActive++;
					}
				}
				if(NumActive == 0)
				{
					for(auto &Group : Map()->m_vpGroups)
					{
						Group->m_Visible = true;
					}
				}
			}

			str_format(aBuf, sizeof(aBuf), "#%d %s", g, Map()->m_vpGroups[g]->m_aName);

			bool Clicked;
			bool Abrupted;
			if(int Result = DoButton_DraggableEx(Map()->m_vpGroups[g].get(), aBuf, g == Map()->m_SelectedGroup, &Slot, &Clicked, &Abrupted,
				   BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT, Map()->m_vpGroups[g]->m_Collapse ? "Select group. Shift+left click to select all layers. Double click to expand." : "Select group. Shift+left click to select all layers. Double click to collapse.", IGraphics::CORNER_R))
			{
				if(s_Operation == OP_NONE)
				{
					s_InitialMouseY = Ui()->MouseY();
					s_InitialCutHeight = s_InitialMouseY - UnscrolledLayersBox.y;
					SetOperation(OP_CLICK);

					if(g != Map()->m_SelectedGroup)
						Map()->SelectLayer(0, g);
				}

				if(Abrupted)
				{
					SetOperation(OP_NONE);
				}

				if(s_Operation == OP_CLICK && absolute(Ui()->MouseY() - s_InitialMouseY) > MinDragDistance)
				{
					StartDragGroup = true;
					s_pDraggedButton = Map()->m_vpGroups[g].get();
				}

				if(s_Operation == OP_CLICK && Clicked)
				{
					if(g != Map()->m_SelectedGroup)
						Map()->SelectLayer(0, g);

					if(Input()->ShiftIsPressed() && Map()->m_SelectedGroup == g)
					{
						Map()->m_vSelectedLayers.clear();
						for(size_t i = 0; i < Map()->m_vpGroups[g]->m_vpLayers.size(); i++)
						{
							Map()->AddSelectedLayer(i);
						}
					}

					if(Result == 2)
					{
						static SPopupMenuId s_PopupGroupId;
						Ui()->DoPopupMenu(&s_PopupGroupId, Ui()->MouseX(), Ui()->MouseY(), 145, 256, this, PopupGroup);
					}

					if(!Map()->m_vpGroups[g]->m_vpLayers.empty() && Ui()->DoDoubleClickLogic(Map()->m_vpGroups[g].get()))
						Map()->m_vpGroups[g]->m_Collapse ^= 1;

					SetOperation(OP_NONE);
				}

				if(s_Operation == OP_GROUP_DRAG && Clicked)
					MoveGroup = true;
			}
			else if(s_pDraggedButton == Map()->m_vpGroups[g].get())
			{
				SetOperation(OP_NONE);
			}
		}

		for(int i = 0; i < (int)Map()->m_vpGroups[g]->m_vpLayers.size(); i++)
		{
			if(Map()->m_vpGroups[g]->m_Collapse)
				continue;

			bool IsLayerSelected = false;
			if(Map()->m_SelectedGroup == g)
			{
				for(const auto &Selected : Map()->m_vSelectedLayers)
				{
					if(Selected == i)
					{
						IsLayerSelected = true;
						break;
					}
				}
			}

			if(s_Operation == OP_GROUP_DRAG && g == Map()->m_SelectedGroup)
			{
				UnscrolledLayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &UnscrolledLayersBox);
			}
			else if(s_Operation == OP_LAYER_DRAG)
			{
				if(IsLayerSelected)
				{
					UnscrolledLayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &UnscrolledLayersBox);
				}
				else
				{
					if(!DraggedPositionFound && Ui()->MouseY() < LayersBox.y + RowHeight / 2)
					{
						DraggedPositionFound = true;
						GroupAfterDraggedLayer = g + 1;
						LayerAfterDraggedLayer = i;
						for(size_t j = 0; j < Map()->m_vSelectedLayers.size(); j++)
						{
							LayersBox.HSplitTop(RowHeight + 2.0f, nullptr, &LayersBox);
							s_ScrollRegion.AddRect(Slot);
						}
					}
					LayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &LayersBox);
					if(!s_ScrollRegion.AddRect(Slot, ScrollToSelection && IsLayerSelected))
						continue;
				}
			}
			else
			{
				LayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &LayersBox);
				if(!s_ScrollRegion.AddRect(Slot, ScrollToSelection && IsLayerSelected))
					continue;
			}

			Slot.HSplitTop(RowHeight, &Slot, nullptr);

			CUIRect Button;
			Slot.VSplitLeft(12.0f, nullptr, &Slot);
			Slot.VSplitLeft(15.0f, &VisibleToggle, &Button);

			const int MouseClick = DoButton_FontIcon(&Map()->m_vpGroups[g]->m_vpLayers[i]->m_Visible, Map()->m_vpGroups[g]->m_vpLayers[i]->m_Visible ? FontIcon::EYE : FontIcon::EYE_SLASH, 0, &VisibleToggle, BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT, "Left click to toggle visibility. Right click to show only this layer within its group.", IGraphics::CORNER_L, 8.0f);
			if(MouseClick == 1)
			{
				Map()->m_vpGroups[g]->m_vpLayers[i]->m_Visible = !Map()->m_vpGroups[g]->m_vpLayers[i]->m_Visible;
			}
			else if(MouseClick == 2)
			{
				if(Input()->ShiftIsPressed())
				{
					if(!IsLayerSelected)
						Map()->SelectLayer(i, g);
				}

				int NumActive = 0;
				for(auto &Layer : Map()->m_vpGroups[g]->m_vpLayers)
				{
					if(Layer == Map()->m_vpGroups[g]->m_vpLayers[i])
					{
						Layer->m_Visible = true;
						continue;
					}

					if(Layer->m_Visible)
					{
						Layer->m_Visible = false;
						NumActive++;
					}
				}
				if(NumActive == 0)
				{
					for(auto &Layer : Map()->m_vpGroups[g]->m_vpLayers)
					{
						Layer->m_Visible = true;
					}
				}
			}

			if(Map()->m_vpGroups[g]->m_vpLayers[i]->m_aName[0])
				str_copy(aBuf, Map()->m_vpGroups[g]->m_vpLayers[i]->m_aName);
			else
			{
				if(Map()->m_vpGroups[g]->m_vpLayers[i]->m_Type == LAYERTYPE_TILES)
				{
					std::shared_ptr<CLayerTiles> pTiles = std::static_pointer_cast<CLayerTiles>(Map()->m_vpGroups[g]->m_vpLayers[i]);
					str_copy(aBuf, pTiles->m_Image >= 0 ? Map()->m_vpImages[pTiles->m_Image]->m_aName : "Tiles");
				}
				else if(Map()->m_vpGroups[g]->m_vpLayers[i]->m_Type == LAYERTYPE_QUADS)
				{
					std::shared_ptr<CLayerQuads> pQuads = std::static_pointer_cast<CLayerQuads>(Map()->m_vpGroups[g]->m_vpLayers[i]);
					str_copy(aBuf, pQuads->m_Image >= 0 ? Map()->m_vpImages[pQuads->m_Image]->m_aName : "Quads");
				}
				else if(Map()->m_vpGroups[g]->m_vpLayers[i]->m_Type == LAYERTYPE_SOUNDS)
				{
					std::shared_ptr<CLayerSounds> pSounds = std::static_pointer_cast<CLayerSounds>(Map()->m_vpGroups[g]->m_vpLayers[i]);
					str_copy(aBuf, pSounds->m_Sound >= 0 ? Map()->m_vpSounds[pSounds->m_Sound]->m_aName : "Sounds");
				}
			}

			int Checked = IsLayerSelected ? 1 : 0;
			if(Map()->m_vpGroups[g]->m_vpLayers[i]->IsEntitiesLayer())
			{
				Checked += 6;
			}

			bool Clicked;
			bool Abrupted;
			if(int Result = DoButton_DraggableEx(Map()->m_vpGroups[g]->m_vpLayers[i].get(), aBuf, Checked, &Button, &Clicked, &Abrupted,
				   BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT, "Select layer. Hold shift to select multiple.", IGraphics::CORNER_R))
			{
				if(s_Operation == OP_NONE)
				{
					s_InitialMouseY = Ui()->MouseY();
					s_InitialCutHeight = s_InitialMouseY - UnscrolledLayersBox.y;

					SetOperation(OP_CLICK);

					if(!Input()->ShiftIsPressed() && !IsLayerSelected)
					{
						Map()->SelectLayer(i, g);
					}
				}

				if(Abrupted)
				{
					SetOperation(OP_NONE);
				}

				if(s_Operation == OP_CLICK && absolute(Ui()->MouseY() - s_InitialMouseY) > MinDragDistance)
				{
					bool EntitiesLayerSelected = false;
					for(int k : Map()->m_vSelectedLayers)
					{
						if(Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers[k]->IsEntitiesLayer())
							EntitiesLayerSelected = true;
					}

					if(!EntitiesLayerSelected)
						StartDragLayer = true;

					s_pDraggedButton = Map()->m_vpGroups[g]->m_vpLayers[i].get();
				}

				if(s_Operation == OP_CLICK && Clicked)
				{
					static SLayerPopupContext s_LayerPopupContext = {};
					s_LayerPopupContext.m_pEditor = this;
					if(Result == 1)
					{
						if(Input()->ShiftIsPressed() && Map()->m_SelectedGroup == g)
						{
							auto Position = std::find(Map()->m_vSelectedLayers.begin(), Map()->m_vSelectedLayers.end(), i);
							if(Position != Map()->m_vSelectedLayers.end())
								Map()->m_vSelectedLayers.erase(Position);
							else
								Map()->AddSelectedLayer(i);
						}
						else if(!Input()->ShiftIsPressed())
						{
							Map()->SelectLayer(i, g);
						}
					}
					else if(Result == 2)
					{
						s_LayerPopupContext.m_vpLayers.clear();
						s_LayerPopupContext.m_vLayerIndices.clear();

						if(!IsLayerSelected)
						{
							Map()->SelectLayer(i, g);
						}

						if(Map()->m_vSelectedLayers.size() > 1)
						{
							// move right clicked layer to first index to render correct popup
							if(Map()->m_vSelectedLayers[0] != i)
							{
								auto Position = std::find(Map()->m_vSelectedLayers.begin(), Map()->m_vSelectedLayers.end(), i);
								std::swap(Map()->m_vSelectedLayers[0], *Position);
							}

							bool AllTile = true;
							for(size_t j = 0; AllTile && j < Map()->m_vSelectedLayers.size(); j++)
							{
								int LayerIndex = Map()->m_vSelectedLayers[j];
								if(Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers[LayerIndex]->m_Type == LAYERTYPE_TILES)
								{
									s_LayerPopupContext.m_vpLayers.push_back(std::static_pointer_cast<CLayerTiles>(Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers[Map()->m_vSelectedLayers[j]]));
									s_LayerPopupContext.m_vLayerIndices.push_back(LayerIndex);
								}
								else
									AllTile = false;
							}

							// Don't allow editing if all selected layers are not tile layers
							if(!AllTile)
							{
								s_LayerPopupContext.m_vpLayers.clear();
								s_LayerPopupContext.m_vLayerIndices.clear();
							}
						}

						Ui()->DoPopupMenu(&s_LayerPopupContext, Ui()->MouseX(), Ui()->MouseY(), 150, 300, &s_LayerPopupContext, PopupLayer);
					}

					SetOperation(OP_NONE);
				}

				if(s_Operation == OP_LAYER_DRAG && Clicked)
				{
					MoveLayers = true;
				}
			}
			else if(s_pDraggedButton == Map()->m_vpGroups[g]->m_vpLayers[i].get())
			{
				SetOperation(OP_NONE);
			}
		}

		if(s_Operation != OP_GROUP_DRAG || g != Map()->m_SelectedGroup)
		{
			LayersBox.HSplitTop(5.0f, &Slot, &LayersBox);
			s_ScrollRegion.AddRect(Slot);
		}
	}

	if(!DraggedPositionFound && s_Operation == OP_LAYER_DRAG)
	{
		GroupAfterDraggedLayer = Map()->m_vpGroups.size();
		LayerAfterDraggedLayer = Map()->m_vpGroups[GroupAfterDraggedLayer - 1]->m_vpLayers.size();

		CUIRect TmpSlot;
		LayersBox.HSplitTop(Map()->m_vSelectedLayers.size() * (RowHeight + 2.0f), &TmpSlot, &LayersBox);
		s_ScrollRegion.AddRect(TmpSlot);
	}

	if(!DraggedPositionFound && s_Operation == OP_GROUP_DRAG)
	{
		GroupAfterDraggedLayer = Map()->m_vpGroups.size();

		CUIRect TmpSlot;
		if(Map()->m_vpGroups[Map()->m_SelectedGroup]->m_Collapse)
			LayersBox.HSplitTop(RowHeight + 7.0f, &TmpSlot, &LayersBox);
		else
			LayersBox.HSplitTop(vButtonsPerGroup[Map()->m_SelectedGroup] * (RowHeight + 2.0f) + 5.0f, &TmpSlot, &LayersBox);
		s_ScrollRegion.AddRect(TmpSlot, false);
	}

	if(MoveLayers && 1 <= GroupAfterDraggedLayer && GroupAfterDraggedLayer <= (int)Map()->m_vpGroups.size())
	{
		std::vector<std::shared_ptr<CLayer>> &vpNewGroupLayers = Map()->m_vpGroups[GroupAfterDraggedLayer - 1]->m_vpLayers;
		if(0 <= LayerAfterDraggedLayer && LayerAfterDraggedLayer <= (int)vpNewGroupLayers.size())
		{
			std::vector<std::shared_ptr<CLayer>> vpSelectedLayers;
			std::vector<std::shared_ptr<CLayer>> &vpSelectedGroupLayers = Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers;
			std::shared_ptr<CLayer> pNextLayer = nullptr;
			if(LayerAfterDraggedLayer < (int)vpNewGroupLayers.size())
				pNextLayer = vpNewGroupLayers[LayerAfterDraggedLayer];

			std::sort(Map()->m_vSelectedLayers.begin(), Map()->m_vSelectedLayers.end(), std::greater<>());
			for(int k : Map()->m_vSelectedLayers)
			{
				vpSelectedLayers.insert(vpSelectedLayers.begin(), vpSelectedGroupLayers[k]);
			}
			for(int k : Map()->m_vSelectedLayers)
			{
				vpSelectedGroupLayers.erase(vpSelectedGroupLayers.begin() + k);
			}

			auto InsertPosition = std::find(vpNewGroupLayers.begin(), vpNewGroupLayers.end(), pNextLayer);
			int InsertPositionIndex = InsertPosition - vpNewGroupLayers.begin();
			vpNewGroupLayers.insert(InsertPosition, vpSelectedLayers.begin(), vpSelectedLayers.end());

			int NumSelectedLayers = Map()->m_vSelectedLayers.size();
			Map()->m_vSelectedLayers.clear();
			for(int i = 0; i < NumSelectedLayers; i++)
				Map()->m_vSelectedLayers.push_back(InsertPositionIndex + i);

			Map()->m_SelectedGroup = GroupAfterDraggedLayer - 1;
			Map()->OnModify();
		}
	}

	if(MoveGroup && 0 <= GroupAfterDraggedLayer && GroupAfterDraggedLayer <= (int)Map()->m_vpGroups.size())
	{
		std::shared_ptr<CLayerGroup> pSelectedGroup = Map()->m_vpGroups[Map()->m_SelectedGroup];
		std::shared_ptr<CLayerGroup> pNextGroup = nullptr;
		if(GroupAfterDraggedLayer < (int)Map()->m_vpGroups.size())
			pNextGroup = Map()->m_vpGroups[GroupAfterDraggedLayer];

		Map()->m_vpGroups.erase(Map()->m_vpGroups.begin() + Map()->m_SelectedGroup);

		auto InsertPosition = std::find(Map()->m_vpGroups.begin(), Map()->m_vpGroups.end(), pNextGroup);
		Map()->m_vpGroups.insert(InsertPosition, pSelectedGroup);

		auto Pos = std::find(Map()->m_vpGroups.begin(), Map()->m_vpGroups.end(), pSelectedGroup);
		Map()->m_SelectedGroup = Pos - Map()->m_vpGroups.begin();

		Map()->OnModify();
	}

	static int s_InitialGroupIndex;
	static std::vector<int> s_vInitialLayerIndices;

	if(MoveLayers || MoveGroup)
	{
		SetOperation(OP_NONE);
	}
	if(StartDragLayer)
	{
		SetOperation(OP_LAYER_DRAG);
		s_InitialGroupIndex = Map()->m_SelectedGroup;
		s_vInitialLayerIndices = std::vector(Map()->m_vSelectedLayers);
	}
	if(StartDragGroup)
	{
		s_InitialGroupIndex = Map()->m_SelectedGroup;
		SetOperation(OP_GROUP_DRAG);
	}

	if(s_Operation == OP_LAYER_DRAG || s_Operation == OP_GROUP_DRAG)
	{
		if(s_pDraggedButton == nullptr)
		{
			SetOperation(OP_NONE);
		}
		else
		{
			s_ScrollRegion.DoEdgeScrolling();
			Ui()->SetActiveItem(s_pDraggedButton);
		}
	}

	if(Input()->KeyPress(KEY_DOWN) && m_Dialog == DIALOG_NONE && !Ui()->IsPopupOpen() && CLineInput::GetActiveInput() == nullptr && s_Operation == OP_NONE)
	{
		if(Input()->ShiftIsPressed())
		{
			if(Map()->m_vSelectedLayers[Map()->m_vSelectedLayers.size() - 1] < (int)Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers.size() - 1)
				Map()->AddSelectedLayer(Map()->m_vSelectedLayers[Map()->m_vSelectedLayers.size() - 1] + 1);
		}
		else
		{
			Map()->SelectNextLayer();
		}
		s_ScrollToSelectionNext = true;
	}
	if(Input()->KeyPress(KEY_UP) && m_Dialog == DIALOG_NONE && !Ui()->IsPopupOpen() && CLineInput::GetActiveInput() == nullptr && s_Operation == OP_NONE)
	{
		if(Input()->ShiftIsPressed())
		{
			if(Map()->m_vSelectedLayers[Map()->m_vSelectedLayers.size() - 1] > 0)
				Map()->AddSelectedLayer(Map()->m_vSelectedLayers[Map()->m_vSelectedLayers.size() - 1] - 1);
		}
		else
		{
			Map()->SelectPreviousLayer();
		}

		s_ScrollToSelectionNext = true;
	}

	CUIRect AddGroupButton, CollapseAllButton;
	LayersBox.HSplitTop(RowHeight + 1.0f, &AddGroupButton, &LayersBox);
	if(s_ScrollRegion.AddRect(AddGroupButton))
	{
		AddGroupButton.HSplitTop(RowHeight, &AddGroupButton, nullptr);
		if(DoButton_Editor(&m_QuickActionAddGroup, m_QuickActionAddGroup.Label(), 0, &AddGroupButton, BUTTONFLAG_LEFT, m_QuickActionAddGroup.Description()))
		{
			m_QuickActionAddGroup.Call();
		}
	}

	LayersBox.HSplitTop(5.0f, nullptr, &LayersBox);
	LayersBox.HSplitTop(RowHeight + 1.0f, &CollapseAllButton, &LayersBox);
	if(s_ScrollRegion.AddRect(CollapseAllButton))
	{
		size_t TotalCollapsed = 0;
		for(const auto &pGroup : Map()->m_vpGroups)
		{
			if(pGroup->m_vpLayers.empty() || pGroup->m_Collapse)
			{
				TotalCollapsed++;
			}
		}

		const char *pActionText = TotalCollapsed == Map()->m_vpGroups.size() ? "Expand all" : "Collapse all";

		CollapseAllButton.HSplitTop(RowHeight, &CollapseAllButton, nullptr);
		static int s_CollapseAllButton = 0;
		if(DoButton_Editor(&s_CollapseAllButton, pActionText, 0, &CollapseAllButton, BUTTONFLAG_LEFT, "Expand or collapse all groups."))
		{
			for(const auto &pGroup : Map()->m_vpGroups)
			{
				if(TotalCollapsed == Map()->m_vpGroups.size())
					pGroup->m_Collapse = false;
				else if(!pGroup->m_vpLayers.empty())
					pGroup->m_Collapse = true;
			}
		}
	}

	s_ScrollRegion.End();

	if(s_Operation == OP_NONE)
	{
		if(s_PreviousOperation == OP_GROUP_DRAG)
		{
			s_PreviousOperation = OP_NONE;
			Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionEditGroupProp>(Map(), Map()->m_SelectedGroup, EGroupProp::ORDER, s_InitialGroupIndex, Map()->m_SelectedGroup));
		}
		else if(s_PreviousOperation == OP_LAYER_DRAG)
		{
			if(s_InitialGroupIndex != Map()->m_SelectedGroup)
			{
				Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionEditLayersGroupAndOrder>(Map(), s_InitialGroupIndex, s_vInitialLayerIndices, Map()->m_SelectedGroup, Map()->m_vSelectedLayers));
			}
			else
			{
				std::vector<std::shared_ptr<IEditorAction>> vpActions;
				std::vector<int> vLayerIndices = Map()->m_vSelectedLayers;
				std::sort(vLayerIndices.begin(), vLayerIndices.end());
				std::sort(s_vInitialLayerIndices.begin(), s_vInitialLayerIndices.end());
				for(int k = 0; k < (int)vLayerIndices.size(); k++)
				{
					int LayerIndex = vLayerIndices[k];
					vpActions.push_back(std::make_shared<CEditorActionEditLayerProp>(Map(), Map()->m_SelectedGroup, LayerIndex, ELayerProp::ORDER, s_vInitialLayerIndices[k], LayerIndex));
				}
				Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionBulk>(Map(), vpActions, nullptr, true));
			}
			s_PreviousOperation = OP_NONE;
		}
	}
}

bool CEditor::ReplaceImage(const char *pFilename, int StorageType, bool CheckDuplicate)
{
	// check if we have that image already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFilename, aBuf, sizeof(aBuf));
	if(CheckDuplicate)
	{
		for(const auto &pImage : Map()->m_vpImages)
		{
			if(!str_comp(pImage->m_aName, aBuf))
			{
				ShowFileDialogError("Image named '%s' was already added.", pImage->m_aName);
				return false;
			}
		}
	}

	CImageInfo ImgInfo;
	if(!Graphics()->LoadPng(ImgInfo, pFilename, StorageType))
	{
		ShowFileDialogError("Failed to load image from file '%s'.", pFilename);
		return false;
	}

	std::shared_ptr<CEditorImage> pImg = Map()->SelectedImage();
	pImg->CEditorImage::Free();
	pImg->m_Width = ImgInfo.m_Width;
	pImg->m_Height = ImgInfo.m_Height;
	pImg->m_Format = ImgInfo.m_Format;
	pImg->m_pData = ImgInfo.m_pData;
	str_copy(pImg->m_aName, aBuf);
	pImg->m_External = IsVanillaImage(pImg->m_aName);

	ConvertToRgba(*pImg);
	DilateImage(*pImg);

	pImg->m_AutoMapper.Load(pImg->m_aName);
	int TextureLoadFlag = Graphics()->Uses2DTextureArrays() ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE : IGraphics::TEXLOAD_TO_3D_TEXTURE;
	if(pImg->m_Width % 16 != 0 || pImg->m_Height % 16 != 0)
		TextureLoadFlag = 0;
	pImg->m_Texture = Graphics()->LoadTextureRaw(*pImg, TextureLoadFlag, pFilename);

	Map()->SortImages();
	Map()->SelectImage(pImg);
	OnDialogClose();
	return true;
}

bool CEditor::ReplaceImageCallback(const char *pFilename, int StorageType, void *pUser)
{
	return static_cast<CEditor *>(pUser)->ReplaceImage(pFilename, StorageType, true);
}

bool CEditor::AddImage(const char *pFilename, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;

	// check if we have that image already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFilename, aBuf, sizeof(aBuf));
	for(const auto &pImage : pEditor->Map()->m_vpImages)
	{
		if(!str_comp(pImage->m_aName, aBuf))
		{
			pEditor->ShowFileDialogError("Image named '%s' was already added.", pImage->m_aName);
			return false;
		}
	}

	if(pEditor->Map()->m_vpImages.size() >= MAX_MAPIMAGES)
	{
		pEditor->m_PopupEventType = POPEVENT_IMAGE_MAX;
		pEditor->m_PopupEventActivated = true;
		return false;
	}

	CImageInfo ImgInfo;
	if(!pEditor->Graphics()->LoadPng(ImgInfo, pFilename, StorageType))
	{
		pEditor->ShowFileDialogError("Failed to load image from file '%s'.", pFilename);
		return false;
	}

	std::shared_ptr<CEditorImage> pImg = std::make_shared<CEditorImage>(pEditor->Map());
	pImg->m_Width = ImgInfo.m_Width;
	pImg->m_Height = ImgInfo.m_Height;
	pImg->m_Format = ImgInfo.m_Format;
	pImg->m_pData = ImgInfo.m_pData;
	pImg->m_External = IsVanillaImage(aBuf);

	ConvertToRgba(*pImg);
	DilateImage(*pImg);

	int TextureLoadFlag = pEditor->Graphics()->Uses2DTextureArrays() ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE : IGraphics::TEXLOAD_TO_3D_TEXTURE;
	if(pImg->m_Width % 16 != 0 || pImg->m_Height % 16 != 0)
		TextureLoadFlag = 0;
	pImg->m_Texture = pEditor->Graphics()->LoadTextureRaw(*pImg, TextureLoadFlag, pFilename);
	str_copy(pImg->m_aName, aBuf);
	pImg->m_AutoMapper.Load(pImg->m_aName);
	pEditor->Map()->m_vpImages.push_back(pImg);
	pEditor->Map()->SortImages();
	pEditor->Map()->SelectImage(pImg);
	pEditor->OnDialogClose();
	return true;
}

bool CEditor::AddSound(const char *pFilename, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;

	// check if we have that sound already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFilename, aBuf, sizeof(aBuf));
	for(const auto &pSound : pEditor->Map()->m_vpSounds)
	{
		if(!str_comp(pSound->m_aName, aBuf))
		{
			pEditor->ShowFileDialogError("Sound named '%s' was already added.", pSound->m_aName);
			return false;
		}
	}

	if(pEditor->Map()->m_vpSounds.size() >= MAX_MAPSOUNDS)
	{
		pEditor->m_PopupEventType = POPEVENT_SOUND_MAX;
		pEditor->m_PopupEventActivated = true;
		return false;
	}

	// load external
	void *pData;
	unsigned DataSize;
	if(!pEditor->Storage()->ReadFile(pFilename, StorageType, &pData, &DataSize))
	{
		pEditor->ShowFileDialogError("Failed to open sound file '%s'.", pFilename);
		return false;
	}

	// load sound
	const int SoundId = pEditor->Sound()->LoadOpusFromMem(pData, DataSize, true, pFilename);
	if(SoundId == -1)
	{
		free(pData);
		pEditor->ShowFileDialogError("Failed to load sound from file '%s'.", pFilename);
		return false;
	}

	// add sound
	std::shared_ptr<CEditorSound> pSound = std::make_shared<CEditorSound>(pEditor->Map());
	pSound->m_SoundId = SoundId;
	pSound->m_DataSize = DataSize;
	pSound->m_pData = pData;
	str_copy(pSound->m_aName, aBuf);
	pEditor->Map()->m_vpSounds.push_back(pSound);

	pEditor->Map()->SelectSound(pSound);
	pEditor->OnDialogClose();
	return true;
}

bool CEditor::ReplaceSound(const char *pFilename, int StorageType, bool CheckDuplicate)
{
	// check if we have that sound already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFilename, aBuf, sizeof(aBuf));
	if(CheckDuplicate)
	{
		for(const auto &pSound : Map()->m_vpSounds)
		{
			if(!str_comp(pSound->m_aName, aBuf))
			{
				ShowFileDialogError("Sound named '%s' was already added.", pSound->m_aName);
				return false;
			}
		}
	}

	// load external
	void *pData;
	unsigned DataSize;
	if(!Storage()->ReadFile(pFilename, StorageType, &pData, &DataSize))
	{
		ShowFileDialogError("Failed to open sound file '%s'.", pFilename);
		return false;
	}

	// load sound
	const int SoundId = Sound()->LoadOpusFromMem(pData, DataSize, true, pFilename);
	if(SoundId == -1)
	{
		free(pData);
		ShowFileDialogError("Failed to load sound from file '%s'.", pFilename);
		return false;
	}

	std::shared_ptr<CEditorSound> pSound = Map()->SelectedSound();

	if(m_ToolbarPreviewSound == pSound->m_SoundId)
	{
		m_ToolbarPreviewSound = SoundId;
	}

	// unload sample
	Sound()->UnloadSample(pSound->m_SoundId);
	free(pSound->m_pData);

	// replace sound
	str_copy(pSound->m_aName, aBuf);
	pSound->m_SoundId = SoundId;
	pSound->m_pData = pData;
	pSound->m_DataSize = DataSize;

	Map()->SelectSound(pSound);
	OnDialogClose();
	return true;
}

bool CEditor::ReplaceSoundCallback(const char *pFilename, int StorageType, void *pUser)
{
	return static_cast<CEditor *>(pUser)->ReplaceSound(pFilename, StorageType, true);
}

void CEditor::RenderImagesList(CUIRect ToolBox)
{
	const float RowHeight = 12.0f;

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollbarWidth = 10.0f;
	ScrollParams.m_ScrollbarMargin = 3.0f;
	ScrollParams.m_ScrollUnit = RowHeight * 5;
	s_ScrollRegion.Begin(&ToolBox, &ScrollOffset, &ScrollParams);
	ToolBox.y += ScrollOffset.y;

	bool ScrollToSelection = false;
	if(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && !Map()->m_vpImages.empty())
	{
		if(Input()->KeyPress(KEY_DOWN))
		{
			const int OldImage = Map()->m_SelectedImage;
			Map()->SelectNextImage();
			ScrollToSelection = OldImage != Map()->m_SelectedImage;
		}
		else if(Input()->KeyPress(KEY_UP))
		{
			const int OldImage = Map()->m_SelectedImage;
			Map()->SelectPreviousImage();
			ScrollToSelection = OldImage != Map()->m_SelectedImage;
		}
	}

	for(int e = 0; e < 2; e++) // two passes, first embedded, then external
	{
		CUIRect Slot;
		ToolBox.HSplitTop(RowHeight + 3.0f, &Slot, &ToolBox);
		if(s_ScrollRegion.AddRect(Slot))
			Ui()->DoLabel(&Slot, e == 0 ? "Embedded" : "External", 12.0f, TEXTALIGN_MC);

		for(int i = 0; i < (int)Map()->m_vpImages.size(); i++)
		{
			if((e && !Map()->m_vpImages[i]->m_External) ||
				(!e && Map()->m_vpImages[i]->m_External))
			{
				continue;
			}

			ToolBox.HSplitTop(RowHeight + 2.0f, &Slot, &ToolBox);
			int Selected = Map()->m_SelectedImage == i;
			if(!s_ScrollRegion.AddRect(Slot, Selected && ScrollToSelection))
				continue;
			Slot.HSplitTop(RowHeight, &Slot, nullptr);

			const bool ImageUsed = std::any_of(Map()->m_vpGroups.cbegin(), Map()->m_vpGroups.cend(), [i](const auto &pGroup) {
				return std::any_of(pGroup->m_vpLayers.cbegin(), pGroup->m_vpLayers.cend(), [i](const auto &pLayer) {
					if(pLayer->m_Type == LAYERTYPE_QUADS)
						return std::static_pointer_cast<CLayerQuads>(pLayer)->m_Image == i;
					else if(pLayer->m_Type == LAYERTYPE_TILES)
						return std::static_pointer_cast<CLayerTiles>(pLayer)->m_Image == i;
					return false;
				});
			});

			if(!ImageUsed)
				Selected += 2; // Image is unused

			if(Selected < 2 && e == 1)
			{
				if(!IsVanillaImage(Map()->m_vpImages[i]->m_aName))
				{
					Selected += 4; // Image should be embedded
				}
			}

			if(int Result = DoButton_Ex(&Map()->m_vpImages[i], Map()->m_vpImages[i]->m_aName, Selected, &Slot,
				   BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT, "Select image.", IGraphics::CORNER_ALL))
			{
				Map()->m_SelectedImage = i;

				if(Result == 2)
				{
					const int Height = Map()->SelectedImage()->m_External ? 73 : 107;
					static SPopupMenuId s_PopupImageId;
					Ui()->DoPopupMenu(&s_PopupImageId, Ui()->MouseX(), Ui()->MouseY(), 140, Height, this, PopupImage);
				}
			}
		}

		// separator
		ToolBox.HSplitTop(5.0f, &Slot, &ToolBox);
		if(s_ScrollRegion.AddRect(Slot))
		{
			IGraphics::CLineItem LineItem(Slot.x, Slot.y + Slot.h / 2, Slot.x + Slot.w, Slot.y + Slot.h / 2);
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->LinesDraw(&LineItem, 1);
			Graphics()->LinesEnd();
		}
	}

	// new image
	static int s_AddImageButton = 0;
	CUIRect AddImageButton;
	ToolBox.HSplitTop(5.0f + RowHeight + 1.0f, &AddImageButton, &ToolBox);
	if(s_ScrollRegion.AddRect(AddImageButton))
	{
		AddImageButton.HSplitTop(5.0f, nullptr, &AddImageButton);
		AddImageButton.HSplitTop(RowHeight, &AddImageButton, nullptr);
		if(DoButton_Editor(&s_AddImageButton, m_QuickActionAddImage.Label(), 0, &AddImageButton, BUTTONFLAG_LEFT, m_QuickActionAddImage.Description()))
			m_QuickActionAddImage.Call();
	}
	s_ScrollRegion.End();
}

void CEditor::RenderSelectedImage(CUIRect View) const
{
	std::shared_ptr<CEditorImage> pSelectedImage = Map()->SelectedImage();
	if(pSelectedImage == nullptr)
		return;

	View.Margin(10.0f, &View);
	if(View.h < View.w)
		View.w = View.h;
	else
		View.h = View.w;
	float Max = maximum<float>(pSelectedImage->m_Width, pSelectedImage->m_Height);
	View.w *= pSelectedImage->m_Width / Max;
	View.h *= pSelectedImage->m_Height / Max;
	Graphics()->TextureSet(pSelectedImage->m_Texture);
	Graphics()->BlendNormal();
	Graphics()->WrapClamp();
	Graphics()->QuadsBegin();
	IGraphics::CQuadItem QuadItem(View.x, View.y, View.w, View.h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();
}

void CEditor::RenderSounds(CUIRect ToolBox)
{
	const float RowHeight = 12.0f;

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollbarWidth = 10.0f;
	ScrollParams.m_ScrollbarMargin = 3.0f;
	ScrollParams.m_ScrollUnit = RowHeight * 5;
	s_ScrollRegion.Begin(&ToolBox, &ScrollOffset, &ScrollParams);
	ToolBox.y += ScrollOffset.y;

	bool ScrollToSelection = false;
	if(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && !Map()->m_vpSounds.empty())
	{
		if(Input()->KeyPress(KEY_DOWN))
		{
			Map()->SelectNextSound();
			ScrollToSelection = true;
		}
		else if(Input()->KeyPress(KEY_UP))
		{
			Map()->SelectPreviousSound();
			ScrollToSelection = true;
		}
	}

	CUIRect Slot;
	ToolBox.HSplitTop(RowHeight + 3.0f, &Slot, &ToolBox);
	if(s_ScrollRegion.AddRect(Slot))
		Ui()->DoLabel(&Slot, "Embedded", 12.0f, TEXTALIGN_MC);

	for(int i = 0; i < (int)Map()->m_vpSounds.size(); i++)
	{
		ToolBox.HSplitTop(RowHeight + 2.0f, &Slot, &ToolBox);
		int Selected = Map()->m_SelectedSound == i;
		if(!s_ScrollRegion.AddRect(Slot, Selected && ScrollToSelection))
			continue;
		Slot.HSplitTop(RowHeight, &Slot, nullptr);

		const bool SoundUsed = std::any_of(Map()->m_vpGroups.cbegin(), Map()->m_vpGroups.cend(), [i](const auto &pGroup) {
			return std::any_of(pGroup->m_vpLayers.cbegin(), pGroup->m_vpLayers.cend(), [i](const auto &pLayer) {
				if(pLayer->m_Type == LAYERTYPE_SOUNDS)
					return std::static_pointer_cast<CLayerSounds>(pLayer)->m_Sound == i;
				return false;
			});
		});

		if(!SoundUsed)
			Selected += 2; // Sound is unused

		if(int Result = DoButton_Ex(&Map()->m_vpSounds[i], Map()->m_vpSounds[i]->m_aName, Selected, &Slot,
			   BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT, "Select sound.", IGraphics::CORNER_ALL))
		{
			Map()->m_SelectedSound = i;

			if(Result == 2)
			{
				static SPopupMenuId s_PopupSoundId;
				Ui()->DoPopupMenu(&s_PopupSoundId, Ui()->MouseX(), Ui()->MouseY(), 140, 90, this, PopupSound);
			}
		}
	}

	// separator
	ToolBox.HSplitTop(5.0f, &Slot, &ToolBox);
	if(s_ScrollRegion.AddRect(Slot))
	{
		IGraphics::CLineItem LineItem(Slot.x, Slot.y + Slot.h / 2, Slot.x + Slot.w, Slot.y + Slot.h / 2);
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->LinesDraw(&LineItem, 1);
		Graphics()->LinesEnd();
	}

	// new sound
	static int s_AddSoundButton = 0;
	CUIRect AddSoundButton;
	ToolBox.HSplitTop(5.0f + RowHeight + 1.0f, &AddSoundButton, &ToolBox);
	if(s_ScrollRegion.AddRect(AddSoundButton))
	{
		AddSoundButton.HSplitTop(5.0f, nullptr, &AddSoundButton);
		AddSoundButton.HSplitTop(RowHeight, &AddSoundButton, nullptr);
		if(DoButton_Editor(&s_AddSoundButton, "Add sound", 0, &AddSoundButton, BUTTONFLAG_LEFT, "Load a new sound to use in the map."))
			m_FileBrowser.ShowFileDialog(IStorage::TYPE_ALL, CFileBrowser::EFileType::SOUND, "Add sound", "Add", "mapres", "", AddSound, this);
	}
	s_ScrollRegion.End();
}

bool CEditor::CStringKeyComparator::operator()(const char *pLhs, const char *pRhs) const
{
	return str_comp(pLhs, pRhs) < 0;
}

void CEditor::ShowFileDialogError(const char *pFormat, ...)
{
	char aMessage[1024];
	va_list VarArgs;
	va_start(VarArgs, pFormat);
	str_format_v(aMessage, sizeof(aMessage), pFormat, VarArgs);
	va_end(VarArgs);

	auto ContextIterator = m_PopupMessageContexts.find(aMessage);
	CUi::SMessagePopupContext *pContext;
	if(ContextIterator != m_PopupMessageContexts.end())
	{
		pContext = ContextIterator->second;
		Ui()->ClosePopupMenu(pContext);
	}
	else
	{
		pContext = new CUi::SMessagePopupContext();
		pContext->ErrorColor();
		str_copy(pContext->m_aMessage, aMessage);
		m_PopupMessageContexts[pContext->m_aMessage] = pContext;
	}
	Ui()->ShowPopupMessage(Ui()->MouseX(), Ui()->MouseY(), pContext);
}

void CEditor::RenderModebar(CUIRect View)
{
	CUIRect Mentions, IngameMoved, ModeButtons, ModeButton;
	View.HSplitTop(12.0f, &Mentions, &View);
	View.HSplitTop(12.0f, &IngameMoved, &View);
	View.HSplitTop(8.0f, nullptr, &ModeButtons);
	const float Width = m_ToolBoxWidth - 5.0f;
	ModeButtons.VSplitLeft(Width, &ModeButtons, nullptr);
	const float ButtonWidth = Width / 3;

	// mentions
	if(m_Mentions)
	{
		char aBuf[64];
		if(m_Mentions == 1)
			str_copy(aBuf, Localize("1 new mention"));
		else if(m_Mentions <= 9)
			str_format(aBuf, sizeof(aBuf), Localize("%d new mentions"), m_Mentions);
		else
			str_copy(aBuf, Localize("9+ new mentions"));

		TextRender()->TextColor(ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f));
		Ui()->DoLabel(&Mentions, aBuf, 10.0f, TEXTALIGN_MC);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	// ingame moved warning
	if(m_IngameMoved)
	{
		TextRender()->TextColor(ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f));
		Ui()->DoLabel(&IngameMoved, Localize("Moved ingame"), 10.0f, TEXTALIGN_MC);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	// mode buttons
	{
		ModeButtons.VSplitLeft(ButtonWidth, &ModeButton, &ModeButtons);
		static int s_LayersButton = 0;
		if(DoButton_FontIcon(&s_LayersButton, FontIcon::LAYER_GROUP, m_Mode == MODE_LAYERS, &ModeButton, BUTTONFLAG_LEFT, "Go to layers management.", IGraphics::CORNER_L))
		{
			m_Mode = MODE_LAYERS;
		}

		ModeButtons.VSplitLeft(ButtonWidth, &ModeButton, &ModeButtons);
		static int s_ImagesButton = 0;
		if(DoButton_FontIcon(&s_ImagesButton, FontIcon::IMAGE, m_Mode == MODE_IMAGES, &ModeButton, BUTTONFLAG_LEFT, "Go to images management.", IGraphics::CORNER_NONE))
		{
			m_Mode = MODE_IMAGES;
		}

		ModeButtons.VSplitLeft(ButtonWidth, &ModeButton, &ModeButtons);
		static int s_SoundsButton = 0;
		if(DoButton_FontIcon(&s_SoundsButton, FontIcon::MUSIC, m_Mode == MODE_SOUNDS, &ModeButton, BUTTONFLAG_LEFT, "Go to sounds management.", IGraphics::CORNER_R))
		{
			m_Mode = MODE_SOUNDS;
		}

		if(Input()->KeyPress(KEY_LEFT) && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
		{
			m_Mode = (m_Mode + NUM_MODES - 1) % NUM_MODES;
		}
		else if(Input()->KeyPress(KEY_RIGHT) && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
		{
			m_Mode = (m_Mode + 1) % NUM_MODES;
		}
	}
}

void CEditor::RenderStatusbar(CUIRect View, CUIRect *pTooltipRect)
{
	CUIRect Button;
	View.VSplitRight(100.0f, &View, &Button);
	if(DoButton_Editor(&m_QuickActionEnvelopes, m_QuickActionEnvelopes.Label(), m_QuickActionEnvelopes.Color(), &Button, BUTTONFLAG_LEFT, m_QuickActionEnvelopes.Description()))
	{
		m_QuickActionEnvelopes.Call();
	}

	View.VSplitRight(10.0f, &View, nullptr);
	View.VSplitRight(100.0f, &View, &Button);
	if(DoButton_Editor(&m_QuickActionServerSettings, m_QuickActionServerSettings.Label(), m_QuickActionServerSettings.Color(), &Button, BUTTONFLAG_LEFT, m_QuickActionServerSettings.Description()))
	{
		m_QuickActionServerSettings.Call();
	}

	View.VSplitRight(10.0f, &View, nullptr);
	View.VSplitRight(100.0f, &View, &Button);
	if(DoButton_Editor(&m_QuickActionHistory, m_QuickActionHistory.Label(), m_QuickActionHistory.Color(), &Button, BUTTONFLAG_LEFT, m_QuickActionHistory.Description()))
	{
		m_QuickActionHistory.Call();
	}

	View.VSplitRight(10.0f, pTooltipRect, nullptr);
}

void CEditor::RenderTooltip(CUIRect TooltipRect)
{
	if(str_comp(m_aTooltip, "") == 0)
		return;

	char aBuf[256];
	if(m_pUiGotContext && m_pUiGotContext == Ui()->HotItem())
		str_format(aBuf, sizeof(aBuf), "%s Right click for context menu.", m_aTooltip);
	else
		str_copy(aBuf, m_aTooltip);

	SLabelProperties Props;
	Props.m_MaxWidth = TooltipRect.w;
	Props.m_EllipsisAtEnd = true;
	Ui()->DoLabel(&TooltipRect, aBuf, 10.0f, TEXTALIGN_ML, Props);
}

void CEditor::ZoomAdaptOffsetX(float ZoomFactor, const CUIRect &View)
{
	float PosX = g_Config.m_EdZoomTarget ? (Ui()->MouseX() - View.x) / View.w : 0.5f;
	m_OffsetEnvelopeX = PosX - (PosX - m_OffsetEnvelopeX) * ZoomFactor;
}

void CEditor::UpdateZoomEnvelopeX(const CUIRect &View)
{
	float OldZoom = m_ZoomEnvelopeX.GetValue();
	if(m_ZoomEnvelopeX.UpdateValue())
		ZoomAdaptOffsetX(OldZoom / m_ZoomEnvelopeX.GetValue(), View);
}

void CEditor::ZoomAdaptOffsetY(float ZoomFactor, const CUIRect &View)
{
	float PosY = g_Config.m_EdZoomTarget ? 1.0f - (Ui()->MouseY() - View.y) / View.h : 0.5f;
	m_OffsetEnvelopeY = PosY - (PosY - m_OffsetEnvelopeY) * ZoomFactor;
}

void CEditor::UpdateZoomEnvelopeY(const CUIRect &View)
{
	float OldZoom = m_ZoomEnvelopeY.GetValue();
	if(m_ZoomEnvelopeY.UpdateValue())
		ZoomAdaptOffsetY(OldZoom / m_ZoomEnvelopeY.GetValue(), View);
}

void CEditor::ResetZoomEnvelope(const std::shared_ptr<CEnvelope> &pEnvelope, int ActiveChannels)
{
	auto [Bottom, Top] = pEnvelope->GetValueRange(ActiveChannels);
	float EndTime = pEnvelope->EndTime();
	float ValueRange = absolute(Top - Bottom);

	if(ValueRange < m_ZoomEnvelopeY.GetMinValue())
	{
		// Set view to some sane default if range is too small
		m_OffsetEnvelopeY = 0.5f - ValueRange / m_ZoomEnvelopeY.GetMinValue() / 2.0f - Bottom / m_ZoomEnvelopeY.GetMinValue();
		m_ZoomEnvelopeY.SetValueInstant(m_ZoomEnvelopeY.GetMinValue());
	}
	else if(ValueRange > m_ZoomEnvelopeY.GetMaxValue())
	{
		m_OffsetEnvelopeY = -Bottom / m_ZoomEnvelopeY.GetMaxValue();
		m_ZoomEnvelopeY.SetValueInstant(m_ZoomEnvelopeY.GetMaxValue());
	}
	else
	{
		// calculate biggest possible spacing
		float SpacingFactor = minimum(1.25f, m_ZoomEnvelopeY.GetMaxValue() / ValueRange);
		m_ZoomEnvelopeY.SetValueInstant(SpacingFactor * ValueRange);
		float Space = 1.0f / SpacingFactor;
		float Spacing = (1.0f - Space) / 2.0f;

		if(Top >= 0 && Bottom >= 0)
			m_OffsetEnvelopeY = Spacing - Bottom / m_ZoomEnvelopeY.GetValue();
		else if(Top <= 0 && Bottom <= 0)
			m_OffsetEnvelopeY = Spacing - Bottom / m_ZoomEnvelopeY.GetValue();
		else
			m_OffsetEnvelopeY = Spacing + Space * absolute(Bottom) / ValueRange;
	}

	if(EndTime < m_ZoomEnvelopeX.GetMinValue())
	{
		m_OffsetEnvelopeX = 0.5f - EndTime / m_ZoomEnvelopeX.GetMinValue();
		m_ZoomEnvelopeX.SetValueInstant(m_ZoomEnvelopeX.GetMinValue());
	}
	else if(EndTime > m_ZoomEnvelopeX.GetMaxValue())
	{
		m_OffsetEnvelopeX = 0.0f;
		m_ZoomEnvelopeX.SetValueInstant(m_ZoomEnvelopeX.GetMaxValue());
	}
	else
	{
		float SpacingFactor = minimum(1.25f, m_ZoomEnvelopeX.GetMaxValue() / EndTime);
		m_ZoomEnvelopeX.SetValueInstant(SpacingFactor * EndTime);
		float Space = 1.0f / SpacingFactor;
		float Spacing = (1.0f - Space) / 2.0f;

		m_OffsetEnvelopeX = Spacing;
	}
}

float CEditor::ScreenToEnvelopeX(const CUIRect &View, float x) const
{
	return (x - View.x - View.w * m_OffsetEnvelopeX) / View.w * m_ZoomEnvelopeX.GetValue();
}

float CEditor::EnvelopeToScreenX(const CUIRect &View, float x) const
{
	return View.x + View.w * m_OffsetEnvelopeX + x / m_ZoomEnvelopeX.GetValue() * View.w;
}

float CEditor::ScreenToEnvelopeY(const CUIRect &View, float y) const
{
	return (View.h - y + View.y) / View.h * m_ZoomEnvelopeY.GetValue() - m_OffsetEnvelopeY * m_ZoomEnvelopeY.GetValue();
}

float CEditor::EnvelopeToScreenY(const CUIRect &View, float y) const
{
	return View.y + View.h - y / m_ZoomEnvelopeY.GetValue() * View.h - m_OffsetEnvelopeY * View.h;
}

float CEditor::ScreenToEnvelopeDX(const CUIRect &View, float DeltaX)
{
	return DeltaX / Graphics()->ScreenWidth() * Ui()->Screen()->w / View.w * m_ZoomEnvelopeX.GetValue();
}

float CEditor::ScreenToEnvelopeDY(const CUIRect &View, float DeltaY)
{
	return DeltaY / Graphics()->ScreenHeight() * Ui()->Screen()->h / View.h * m_ZoomEnvelopeY.GetValue();
}

void CEditor::RemoveTimeOffsetEnvelope(const std::shared_ptr<CEnvelope> &pEnvelope)
{
	CFixedTime TimeOffset = pEnvelope->m_vPoints[0].m_Time;
	for(auto &Point : pEnvelope->m_vPoints)
		Point.m_Time -= TimeOffset;

	m_OffsetEnvelopeX += TimeOffset.AsSeconds() / m_ZoomEnvelopeX.GetValue();
}

static float ClampDelta(float Val, float Delta, float Min, float Max)
{
	if(Val + Delta <= Min)
		return Min - Val;
	if(Val + Delta >= Max)
		return Max - Val;
	return Delta;
}

class CTimeStep
{
public:
	template<class T>
	CTimeStep(T t)
	{
		if constexpr(std::is_same_v<T, std::chrono::milliseconds>)
			m_Unit = ETimeUnit::MILLISECONDS;
		else if constexpr(std::is_same_v<T, std::chrono::seconds>)
			m_Unit = ETimeUnit::SECONDS;
		else
			m_Unit = ETimeUnit::MINUTES;

		m_Value = t;
	}

	CTimeStep operator*(int k) const
	{
		return CTimeStep(m_Value * k, m_Unit);
	}

	CTimeStep operator-(const CTimeStep &Other)
	{
		return CTimeStep(m_Value - Other.m_Value, m_Unit);
	}

	void Format(char *pBuffer, size_t BufferSize)
	{
		int Milliseconds = m_Value.count() % 1000;
		int Seconds = std::chrono::duration_cast<std::chrono::seconds>(m_Value).count() % 60;
		int Minutes = std::chrono::duration_cast<std::chrono::minutes>(m_Value).count();

		switch(m_Unit)
		{
		case ETimeUnit::MILLISECONDS:
			if(Minutes != 0)
				str_format(pBuffer, BufferSize, "%d:%02d.%03dmin", Minutes, Seconds, Milliseconds);
			else if(Seconds != 0)
				str_format(pBuffer, BufferSize, "%d.%03ds", Seconds, Milliseconds);
			else
				str_format(pBuffer, BufferSize, "%dms", Milliseconds);
			break;
		case ETimeUnit::SECONDS:
			if(Minutes != 0)
				str_format(pBuffer, BufferSize, "%d:%02dmin", Minutes, Seconds);
			else
				str_format(pBuffer, BufferSize, "%ds", Seconds);
			break;
		case ETimeUnit::MINUTES:
			str_format(pBuffer, BufferSize, "%dmin", Minutes);
			break;
		}
	}

	float AsSeconds() const
	{
		return std::chrono::duration_cast<std::chrono::duration<float>>(m_Value).count();
	}

private:
	enum class ETimeUnit
	{
		MILLISECONDS,
		SECONDS,
		MINUTES,
	} m_Unit;
	std::chrono::milliseconds m_Value;

	CTimeStep(std::chrono::milliseconds Value, ETimeUnit Unit)
	{
		m_Value = Value;
		m_Unit = Unit;
	}
};

void CEditor::UpdateHotEnvelopePoint(const CUIRect &View, const CEnvelope *pEnvelope, int ActiveChannels)
{
	if(!Ui()->MouseInside(&View))
		return;

	const vec2 MousePos = Ui()->MousePos();

	float MinDist = 200.0f;
	const void *pMinPointId = nullptr;

	const auto UpdateMinimum = [&](vec2 Position, const void *pId) {
		const float CurrDist = length_squared(Position - MousePos);
		if(CurrDist < MinDist)
		{
			MinDist = CurrDist;
			pMinPointId = pId;
		}
	};

	for(size_t i = 0; i < pEnvelope->m_vPoints.size(); i++)
	{
		for(int c = pEnvelope->GetChannels() - 1; c >= 0; c--)
		{
			if(!(ActiveChannels & (1 << c)))
				continue;

			if(i > 0 && pEnvelope->m_vPoints[i - 1].m_Curvetype == CURVETYPE_BEZIER)
			{
				vec2 Position;
				Position.x = EnvelopeToScreenX(View, (pEnvelope->m_vPoints[i].m_Time + pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c]).AsSeconds());
				Position.y = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c] + pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaY[c]));
				UpdateMinimum(Position, &pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c]);
			}

			if(i < pEnvelope->m_vPoints.size() - 1 && pEnvelope->m_vPoints[i].m_Curvetype == CURVETYPE_BEZIER)
			{
				vec2 Position;
				Position.x = EnvelopeToScreenX(View, (pEnvelope->m_vPoints[i].m_Time + pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c]).AsSeconds());
				Position.y = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c] + pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaY[c]));
				UpdateMinimum(Position, &pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c]);
			}

			vec2 Position;
			Position.x = EnvelopeToScreenX(View, pEnvelope->m_vPoints[i].m_Time.AsSeconds());
			Position.y = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c]));
			UpdateMinimum(Position, &pEnvelope->m_vPoints[i].m_aValues[c]);
		}
	}

	if(pMinPointId != nullptr)
	{
		Ui()->SetHotItem(pMinPointId);
	}
}

void CEditor::RenderEnvelopeEditor(CUIRect View)
{
	Map()->m_SelectedEnvelope = Map()->m_vpEnvelopes.empty() ? -1 : std::clamp(Map()->m_SelectedEnvelope, 0, (int)Map()->m_vpEnvelopes.size() - 1);
	std::shared_ptr<CEnvelope> pEnvelope = Map()->m_vpEnvelopes.empty() ? nullptr : Map()->m_vpEnvelopes[Map()->m_SelectedEnvelope];

	static EEnvelopeEditorOp s_Operation = EEnvelopeEditorOp::NONE;
	static std::vector<float> s_vAccurateDragValuesX = {};
	static std::vector<float> s_vAccurateDragValuesY = {};
	static float s_MouseXStart = 0.0f;
	static float s_MouseYStart = 0.0f;

	static CLineInput s_NameInput;

	CUIRect ToolBar, CurveBar, ColorBar, DragBar;
	View.HSplitTop(30.0f, &DragBar, nullptr);
	DragBar.y -= 2.0f;
	DragBar.w += 2.0f;
	DragBar.h += 4.0f;
	DoEditorDragBar(View, &DragBar, EDragSide::TOP, &m_aExtraEditorSplits[EXTRAEDITOR_ENVELOPES]);
	View.HSplitTop(15.0f, &ToolBar, &View);
	View.HSplitTop(15.0f, &CurveBar, &View);
	ToolBar.Margin(2.0f, &ToolBar);
	CurveBar.Margin(2.0f, &CurveBar);

	bool CurrentEnvelopeSwitched = false;

	// do the toolbar
	static int s_ActiveChannels = 0xf;
	{
		CUIRect Button;

		// redo button
		ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
		static int s_RedoButton = 0;
		if(DoButton_FontIcon(&s_RedoButton, FontIcon::REDO, Map()->m_EnvelopeEditorHistory.CanRedo() ? 0 : -1, &Button, BUTTONFLAG_LEFT, "[Ctrl+Y] Redo the last action.", IGraphics::CORNER_R, 11.0f) == 1)
		{
			Map()->m_EnvelopeEditorHistory.Redo();
		}

		// undo button
		ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
		ToolBar.VSplitRight(10.0f, &ToolBar, nullptr);
		static int s_UndoButton = 0;
		if(DoButton_FontIcon(&s_UndoButton, FontIcon::UNDO, Map()->m_EnvelopeEditorHistory.CanUndo() ? 0 : -1, &Button, BUTTONFLAG_LEFT, "[Ctrl+Z] Undo the last action.", IGraphics::CORNER_L, 11.0f) == 1)
		{
			Map()->m_EnvelopeEditorHistory.Undo();
		}

		ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
		static int s_NewSoundButton = 0;
		if(DoButton_Editor(&s_NewSoundButton, "Sound+", 0, &Button, BUTTONFLAG_LEFT, "Create a new sound envelope."))
		{
			Map()->m_EnvelopeEditorHistory.Execute(std::make_shared<CEditorActionEnvelopeAdd>(Map(), CEnvelope::EType::SOUND));
			pEnvelope = Map()->m_vpEnvelopes[Map()->m_SelectedEnvelope];
			CurrentEnvelopeSwitched = true;
		}

		ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
		ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
		static int s_New4dButton = 0;
		if(DoButton_Editor(&s_New4dButton, "Color+", 0, &Button, BUTTONFLAG_LEFT, "Create a new color envelope."))
		{
			Map()->m_EnvelopeEditorHistory.Execute(std::make_shared<CEditorActionEnvelopeAdd>(Map(), CEnvelope::EType::COLOR));
			pEnvelope = Map()->m_vpEnvelopes[Map()->m_SelectedEnvelope];
			CurrentEnvelopeSwitched = true;
		}

		ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
		ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
		static int s_New2dButton = 0;
		if(DoButton_Editor(&s_New2dButton, "Pos.+", 0, &Button, BUTTONFLAG_LEFT, "Create a new position envelope."))
		{
			Map()->m_EnvelopeEditorHistory.Execute(std::make_shared<CEditorActionEnvelopeAdd>(Map(), CEnvelope::EType::POSITION));
			pEnvelope = Map()->m_vpEnvelopes[Map()->m_SelectedEnvelope];
			CurrentEnvelopeSwitched = true;
		}

		if(Map()->m_SelectedEnvelope >= 0)
		{
			// Delete button
			ToolBar.VSplitRight(10.0f, &ToolBar, nullptr);
			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			static int s_DeleteButton = 0;
			if(DoButton_Editor(&s_DeleteButton, "✗", 0, &Button, BUTTONFLAG_LEFT, "Delete this envelope."))
			{
				auto vpObjectReferences = Map()->DeleteEnvelope(Map()->m_SelectedEnvelope);
				Map()->m_EnvelopeEditorHistory.RecordAction(std::make_shared<CEditorActionEnvelopeDelete>(Map(), Map()->m_SelectedEnvelope, vpObjectReferences, pEnvelope));

				Map()->m_SelectedEnvelope = Map()->m_vpEnvelopes.empty() ? -1 : std::clamp(Map()->m_SelectedEnvelope, 0, (int)Map()->m_vpEnvelopes.size() - 1);
				pEnvelope = Map()->m_vpEnvelopes.empty() ? nullptr : Map()->m_vpEnvelopes[Map()->m_SelectedEnvelope];
				Map()->OnModify();
			}
		}

		// check again, because the last envelope might has been deleted
		if(Map()->m_SelectedEnvelope >= 0)
		{
			// Move right button
			ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			static int s_MoveRightButton = 0;
			if(DoButton_Ex(&s_MoveRightButton, "→", (Map()->m_SelectedEnvelope >= (int)Map()->m_vpEnvelopes.size() - 1 ? -1 : 0), &Button, BUTTONFLAG_LEFT, "Move this envelope to the right.", IGraphics::CORNER_R))
			{
				int MoveTo = Map()->m_SelectedEnvelope + 1;
				int MoveFrom = Map()->m_SelectedEnvelope;
				Map()->m_SelectedEnvelope = Map()->MoveEnvelope(MoveFrom, MoveTo);
				if(Map()->m_SelectedEnvelope != MoveFrom)
				{
					Map()->m_EnvelopeEditorHistory.RecordAction(std::make_shared<CEditorActionEnvelopeEdit>(Map(), Map()->m_SelectedEnvelope, CEditorActionEnvelopeEdit::EEditType::ORDER, MoveFrom, Map()->m_SelectedEnvelope));
					pEnvelope = Map()->m_vpEnvelopes[Map()->m_SelectedEnvelope];
					Map()->OnModify();
				}
			}

			// Move left button
			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			static int s_MoveLeftButton = 0;
			if(DoButton_Ex(&s_MoveLeftButton, "←", (Map()->m_SelectedEnvelope <= 0 ? -1 : 0), &Button, BUTTONFLAG_LEFT, "Move this envelope to the left.", IGraphics::CORNER_L))
			{
				int MoveTo = Map()->m_SelectedEnvelope - 1;
				int MoveFrom = Map()->m_SelectedEnvelope;
				Map()->m_SelectedEnvelope = Map()->MoveEnvelope(MoveFrom, MoveTo);
				if(Map()->m_SelectedEnvelope != MoveFrom)
				{
					Map()->m_EnvelopeEditorHistory.RecordAction(std::make_shared<CEditorActionEnvelopeEdit>(Map(), Map()->m_SelectedEnvelope, CEditorActionEnvelopeEdit::EEditType::ORDER, MoveFrom, Map()->m_SelectedEnvelope));
					pEnvelope = Map()->m_vpEnvelopes[Map()->m_SelectedEnvelope];
					Map()->OnModify();
				}
			}

			if(pEnvelope)
			{
				ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
				ToolBar.VSplitRight(20.0f, &ToolBar, &Button);
				static int s_ZoomOutButton = 0;
				if(DoButton_FontIcon(&s_ZoomOutButton, FontIcon::MINUS, 0, &Button, BUTTONFLAG_LEFT, "[NumPad-] Zoom out horizontally, hold shift to zoom vertically.", IGraphics::CORNER_R, 9.0f))
				{
					if(Input()->ShiftIsPressed())
						m_ZoomEnvelopeY.ChangeValue(0.1f * m_ZoomEnvelopeY.GetValue());
					else
						m_ZoomEnvelopeX.ChangeValue(0.1f * m_ZoomEnvelopeX.GetValue());
				}

				ToolBar.VSplitRight(20.0f, &ToolBar, &Button);
				static int s_ResetZoomButton = 0;
				if(DoButton_FontIcon(&s_ResetZoomButton, FontIcon::MAGNIFYING_GLASS, 0, &Button, BUTTONFLAG_LEFT, "[NumPad*] Reset zoom to default value.", IGraphics::CORNER_NONE, 9.0f))
					ResetZoomEnvelope(pEnvelope, s_ActiveChannels);

				ToolBar.VSplitRight(20.0f, &ToolBar, &Button);
				static int s_ZoomInButton = 0;
				if(DoButton_FontIcon(&s_ZoomInButton, FontIcon::PLUS, 0, &Button, BUTTONFLAG_LEFT, "[NumPad+] Zoom in horizontally, hold shift to zoom vertically.", IGraphics::CORNER_L, 9.0f))
				{
					if(Input()->ShiftIsPressed())
						m_ZoomEnvelopeY.ChangeValue(-0.1f * m_ZoomEnvelopeY.GetValue());
					else
						m_ZoomEnvelopeX.ChangeValue(-0.1f * m_ZoomEnvelopeX.GetValue());
				}
			}

			// Margin on the right side
			ToolBar.VSplitRight(7.0f, &ToolBar, nullptr);
		}

		CUIRect Shifter, Inc, Dec;
		ToolBar.VSplitLeft(60.0f, &Shifter, &ToolBar);
		Shifter.VSplitRight(15.0f, &Shifter, &Inc);
		Shifter.VSplitLeft(15.0f, &Dec, &Shifter);
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "%d/%d", Map()->m_SelectedEnvelope + 1, (int)Map()->m_vpEnvelopes.size());

		ColorRGBA EnvColor = ColorRGBA(1, 1, 1, 0.5f);
		if(!Map()->m_vpEnvelopes.empty())
		{
			EnvColor = Map()->IsEnvelopeUsed(Map()->m_SelectedEnvelope) ? ColorRGBA(1, 0.7f, 0.7f, 0.5f) : ColorRGBA(0.7f, 1, 0.7f, 0.5f);
		}

		static int s_EnvelopeSelector = 0;
		auto NewValueRes = UiDoValueSelector(&s_EnvelopeSelector, &Shifter, aBuf, Map()->m_SelectedEnvelope + 1, 1, Map()->m_vpEnvelopes.size(), 1, 1.0f, "Select the envelope.", false, false, IGraphics::CORNER_NONE, &EnvColor, false);
		int NewValue = NewValueRes.m_Value;
		if(NewValue - 1 != Map()->m_SelectedEnvelope)
		{
			Map()->m_SelectedEnvelope = NewValue - 1;
			CurrentEnvelopeSwitched = true;
		}

		static int s_PrevButton = 0;
		if(DoButton_FontIcon(&s_PrevButton, FontIcon::MINUS, 0, &Dec, BUTTONFLAG_LEFT, "Select previous envelope.", IGraphics::CORNER_L, 7.0f))
		{
			Map()->m_SelectedEnvelope--;
			if(Map()->m_SelectedEnvelope < 0)
				Map()->m_SelectedEnvelope = Map()->m_vpEnvelopes.size() - 1;
			CurrentEnvelopeSwitched = true;
		}

		static int s_NextButton = 0;
		if(DoButton_FontIcon(&s_NextButton, FontIcon::PLUS, 0, &Inc, BUTTONFLAG_LEFT, "Select next envelope.", IGraphics::CORNER_R, 7.0f))
		{
			Map()->m_SelectedEnvelope++;
			if(Map()->m_SelectedEnvelope >= (int)Map()->m_vpEnvelopes.size())
				Map()->m_SelectedEnvelope = 0;
			CurrentEnvelopeSwitched = true;
		}

		if(pEnvelope)
		{
			ToolBar.VSplitLeft(15.0f, nullptr, &ToolBar);
			ToolBar.VSplitLeft(40.0f, &Button, &ToolBar);
			Ui()->DoLabel(&Button, "Name:", 10.0f, TEXTALIGN_MR);

			ToolBar.VSplitLeft(3.0f, nullptr, &ToolBar);
			ToolBar.VSplitLeft(ToolBar.w > ToolBar.h * 40 ? 80.0f : 60.0f, &Button, &ToolBar);

			s_NameInput.SetBuffer(pEnvelope->m_aName, sizeof(pEnvelope->m_aName));
			if(DoEditBox(&s_NameInput, &Button, 10.0f, IGraphics::CORNER_ALL, "The name of the selected envelope."))
			{
				Map()->OnModify();
			}
		}
	}

	const bool ShowColorBar = pEnvelope && pEnvelope->GetChannels() == 4;
	if(ShowColorBar)
	{
		View.HSplitTop(20.0f, &ColorBar, &View);
		ColorBar.HMargin(2.0f, &ColorBar);
	}

	RenderBackground(View, m_CheckerTexture, 32.0f, 0.1f);

	if(pEnvelope)
	{
		if(m_ResetZoomEnvelope)
		{
			m_ResetZoomEnvelope = false;
			ResetZoomEnvelope(pEnvelope, s_ActiveChannels);
		}

		ColorRGBA aColors[] = {ColorRGBA(1, 0.2f, 0.2f), ColorRGBA(0.2f, 1, 0.2f), ColorRGBA(0.2f, 0.2f, 1), ColorRGBA(1, 1, 0.2f)};

		CUIRect Button;

		ToolBar.VSplitLeft(15.0f, &Button, &ToolBar);

		static const char *s_aapNames[4][CEnvPoint::MAX_CHANNELS] = {
			{"V", "", "", ""},
			{"", "", "", ""},
			{"X", "Y", "R", ""},
			{"R", "G", "B", "A"},
		};

		static const char *s_aapDescriptions[4][CEnvPoint::MAX_CHANNELS] = {
			{"Volume of the envelope.", "", "", ""},
			{"", "", "", ""},
			{"X-axis of the envelope.", "Y-axis of the envelope.", "Rotation of the envelope.", ""},
			{"Red value of the envelope.", "Green value of the envelope.", "Blue value of the envelope.", "Alpha value of the envelope."},
		};

		static int s_aChannelButtons[CEnvPoint::MAX_CHANNELS] = {0};
		int Bit = 1;

		for(int i = 0; i < CEnvPoint::MAX_CHANNELS; i++, Bit <<= 1)
		{
			ToolBar.VSplitLeft(15.0f, &Button, &ToolBar);
			if(i < pEnvelope->GetChannels())
			{
				int Corners = IGraphics::CORNER_NONE;
				if(pEnvelope->GetChannels() == 1)
					Corners = IGraphics::CORNER_ALL;
				else if(i == 0)
					Corners = IGraphics::CORNER_L;
				else if(i == pEnvelope->GetChannels() - 1)
					Corners = IGraphics::CORNER_R;

				if(DoButton_Env(&s_aChannelButtons[i], s_aapNames[pEnvelope->GetChannels() - 1][i], s_ActiveChannels & Bit, &Button, s_aapDescriptions[pEnvelope->GetChannels() - 1][i], aColors[i], Corners))
					s_ActiveChannels ^= Bit;
			}
		}

		ToolBar.VSplitLeft(15.0f, nullptr, &ToolBar);
		ToolBar.VSplitLeft(40.0f, &Button, &ToolBar);

		static int s_EnvelopeEditorId = 0;
		static int s_EnvelopeEditorButtonUsed = -1;
		const bool ShouldPan = s_Operation == EEnvelopeEditorOp::NONE && (Ui()->MouseButton(2) || (Ui()->MouseButton(0) && Input()->ModifierIsPressed()));
		if(m_pContainerPanned == &s_EnvelopeEditorId)
		{
			if(!ShouldPan)
				m_pContainerPanned = nullptr;
			else
			{
				m_OffsetEnvelopeX += Ui()->MouseDeltaX() / Graphics()->ScreenWidth() * Ui()->Screen()->w / View.w;
				m_OffsetEnvelopeY -= Ui()->MouseDeltaY() / Graphics()->ScreenHeight() * Ui()->Screen()->h / View.h;
			}
		}

		if(Ui()->MouseInside(&View) && m_Dialog == DIALOG_NONE)
		{
			Ui()->SetHotItem(&s_EnvelopeEditorId);

			if(ShouldPan && m_pContainerPanned == nullptr)
				m_pContainerPanned = &s_EnvelopeEditorId;

			if(Input()->KeyPress(KEY_KP_MULTIPLY) && CLineInput::GetActiveInput() == nullptr)
				ResetZoomEnvelope(pEnvelope, s_ActiveChannels);
			if(Input()->ShiftIsPressed())
			{
				if(Input()->KeyPress(KEY_KP_MINUS) && CLineInput::GetActiveInput() == nullptr)
					m_ZoomEnvelopeY.ChangeValue(0.1f * m_ZoomEnvelopeY.GetValue());
				if(Input()->KeyPress(KEY_KP_PLUS) && CLineInput::GetActiveInput() == nullptr)
					m_ZoomEnvelopeY.ChangeValue(-0.1f * m_ZoomEnvelopeY.GetValue());
				if(Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
					m_ZoomEnvelopeY.ChangeValue(0.1f * m_ZoomEnvelopeY.GetValue());
				if(Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
					m_ZoomEnvelopeY.ChangeValue(-0.1f * m_ZoomEnvelopeY.GetValue());
			}
			else
			{
				if(Input()->KeyPress(KEY_KP_MINUS) && CLineInput::GetActiveInput() == nullptr)
					m_ZoomEnvelopeX.ChangeValue(0.1f * m_ZoomEnvelopeX.GetValue());
				if(Input()->KeyPress(KEY_KP_PLUS) && CLineInput::GetActiveInput() == nullptr)
					m_ZoomEnvelopeX.ChangeValue(-0.1f * m_ZoomEnvelopeX.GetValue());
				if(Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
					m_ZoomEnvelopeX.ChangeValue(0.1f * m_ZoomEnvelopeX.GetValue());
				if(Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
					m_ZoomEnvelopeX.ChangeValue(-0.1f * m_ZoomEnvelopeX.GetValue());
			}
		}

		if(Ui()->HotItem() == &s_EnvelopeEditorId)
		{
			// do stuff
			if(Ui()->MouseButton(0))
			{
				s_EnvelopeEditorButtonUsed = 0;
				if(s_Operation != EEnvelopeEditorOp::BOX_SELECT && !Input()->ModifierIsPressed())
				{
					s_Operation = EEnvelopeEditorOp::BOX_SELECT;
					s_MouseXStart = Ui()->MouseX();
					s_MouseYStart = Ui()->MouseY();
				}
			}
			else if(s_EnvelopeEditorButtonUsed == 0)
			{
				if(Ui()->DoDoubleClickLogic(&s_EnvelopeEditorId) && !Input()->ModifierIsPressed())
				{
					// add point
					float Time = ScreenToEnvelopeX(View, Ui()->MouseX());
					ColorRGBA Channels = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
					pEnvelope->Eval(std::clamp(Time, 0.0f, pEnvelope->EndTime()), Channels, 4);

					const CFixedTime FixedTime = CFixedTime::FromSeconds(Time);
					bool TimeFound = false;
					for(CEnvPoint &Point : pEnvelope->m_vPoints)
					{
						if(Point.m_Time == FixedTime)
							TimeFound = true;
					}

					if(!TimeFound)
						Map()->m_EnvelopeEditorHistory.Execute(std::make_shared<CEditorActionAddEnvelopePoint>(Map(), Map()->m_SelectedEnvelope, FixedTime, Channels));

					if(FixedTime < CFixedTime(0))
						RemoveTimeOffsetEnvelope(pEnvelope);
					Map()->OnModify();
				}
				s_EnvelopeEditorButtonUsed = -1;
			}

			m_ActiveEnvelopePreview = EEnvelopePreview::SELECTED;
			str_copy(m_aTooltip, "Double click to create a new point. Use shift to change the zoom axis. Press S to scale selected envelope points.");
		}

		UpdateZoomEnvelopeX(View);
		UpdateZoomEnvelopeY(View);

		{
			float UnitsPerLineY = 0.001f;
			static const float s_aUnitPerLineOptionsY[] = {0.005f, 0.01f, 0.025f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 2 * 32.0f, 5 * 32.0f, 10 * 32.0f, 20 * 32.0f, 50 * 32.0f, 100 * 32.0f};
			for(float Value : s_aUnitPerLineOptionsY)
			{
				if(Value / m_ZoomEnvelopeY.GetValue() * View.h < 40.0f)
					UnitsPerLineY = Value;
			}
			int NumLinesY = m_ZoomEnvelopeY.GetValue() / UnitsPerLineY + 1;

			Ui()->ClipEnable(&View);
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.2f);

			float BaseValue = static_cast<int>(m_OffsetEnvelopeY * m_ZoomEnvelopeY.GetValue() / UnitsPerLineY) * UnitsPerLineY;
			for(int i = 0; i <= NumLinesY; i++)
			{
				float Value = UnitsPerLineY * i - BaseValue;
				IGraphics::CLineItem LineItem(View.x, EnvelopeToScreenY(View, Value), View.x + View.w, EnvelopeToScreenY(View, Value));
				Graphics()->LinesDraw(&LineItem, 1);
			}

			Graphics()->LinesEnd();

			Ui()->TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.4f);
			for(int i = 0; i <= NumLinesY; i++)
			{
				float Value = UnitsPerLineY * i - BaseValue;
				char aValueBuffer[16];
				if(UnitsPerLineY >= 1.0f)
				{
					str_format(aValueBuffer, sizeof(aValueBuffer), "%d", static_cast<int>(Value));
				}
				else
				{
					str_format(aValueBuffer, sizeof(aValueBuffer), "%.3f", Value);
				}
				Ui()->TextRender()->Text(View.x, EnvelopeToScreenY(View, Value) + 4.0f, 8.0f, aValueBuffer);
			}
			Ui()->TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			Ui()->ClipDisable();
		}

		{
			using namespace std::chrono_literals;
			CTimeStep UnitsPerLineX = 1ms;
			static const CTimeStep s_aUnitPerLineOptionsX[] = {5ms, 10ms, 25ms, 50ms, 100ms, 250ms, 500ms, 1s, 2s, 5s, 10s, 15s, 30s, 1min};
			for(CTimeStep Value : s_aUnitPerLineOptionsX)
			{
				if(Value.AsSeconds() / m_ZoomEnvelopeX.GetValue() * View.w < 160.0f)
					UnitsPerLineX = Value;
			}
			int NumLinesX = m_ZoomEnvelopeX.GetValue() / UnitsPerLineX.AsSeconds() + 1;

			Ui()->ClipEnable(&View);
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.2f);

			CTimeStep BaseValue = UnitsPerLineX * static_cast<int>(m_OffsetEnvelopeX * m_ZoomEnvelopeX.GetValue() / UnitsPerLineX.AsSeconds());
			for(int i = 0; i <= NumLinesX; i++)
			{
				float Value = UnitsPerLineX.AsSeconds() * i - BaseValue.AsSeconds();
				IGraphics::CLineItem LineItem(EnvelopeToScreenX(View, Value), View.y, EnvelopeToScreenX(View, Value), View.y + View.h);
				Graphics()->LinesDraw(&LineItem, 1);
			}

			Graphics()->LinesEnd();

			Ui()->TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.4f);
			for(int i = 0; i <= NumLinesX; i++)
			{
				CTimeStep Value = UnitsPerLineX * i - BaseValue;
				if(Value.AsSeconds() >= 0)
				{
					char aValueBuffer[16];
					Value.Format(aValueBuffer, sizeof(aValueBuffer));

					Ui()->TextRender()->Text(EnvelopeToScreenX(View, Value.AsSeconds()) + 1.0f, View.y + View.h - 8.0f, 8.0f, aValueBuffer);
				}
			}
			Ui()->TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			Ui()->ClipDisable();
		}

		// render tangents for bezier curves
		{
			Ui()->ClipEnable(&View);
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			for(int c = 0; c < pEnvelope->GetChannels(); c++)
			{
				if(!(s_ActiveChannels & (1 << c)))
					continue;

				for(int i = 0; i < (int)pEnvelope->m_vPoints.size(); i++)
				{
					float PosX = EnvelopeToScreenX(View, pEnvelope->m_vPoints[i].m_Time.AsSeconds());
					float PosY = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c]));

					// Out-Tangent
					if(i < (int)pEnvelope->m_vPoints.size() - 1 && pEnvelope->m_vPoints[i].m_Curvetype == CURVETYPE_BEZIER)
					{
						float TangentX = EnvelopeToScreenX(View, (pEnvelope->m_vPoints[i].m_Time + pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c]).AsSeconds());
						float TangentY = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c] + pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaY[c]));

						if(Map()->IsTangentOutPointSelected(i, c))
							Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
						else
							Graphics()->SetColor(aColors[c].r, aColors[c].g, aColors[c].b, 0.4f);

						IGraphics::CLineItem LineItem(TangentX, TangentY, PosX, PosY);
						Graphics()->LinesDraw(&LineItem, 1);
					}

					// In-Tangent
					if(i > 0 && pEnvelope->m_vPoints[i - 1].m_Curvetype == CURVETYPE_BEZIER)
					{
						float TangentX = EnvelopeToScreenX(View, (pEnvelope->m_vPoints[i].m_Time + pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c]).AsSeconds());
						float TangentY = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c] + pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaY[c]));

						if(Map()->IsTangentInPointSelected(i, c))
							Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
						else
							Graphics()->SetColor(aColors[c].r, aColors[c].g, aColors[c].b, 0.4f);

						IGraphics::CLineItem LineItem(TangentX, TangentY, PosX, PosY);
						Graphics()->LinesDraw(&LineItem, 1);
					}
				}
			}
			Graphics()->LinesEnd();
			Ui()->ClipDisable();
		}

		// render lines
		{
			float EndTimeTotal = maximum(0.000001f, pEnvelope->EndTime());
			float EndX = std::clamp(EnvelopeToScreenX(View, EndTimeTotal), View.x, View.x + View.w);
			float StartX = std::clamp(View.x + View.w * m_OffsetEnvelopeX, View.x, View.x + View.w);

			float EndTime = ScreenToEnvelopeX(View, EndX);
			float StartTime = ScreenToEnvelopeX(View, StartX);

			Ui()->ClipEnable(&View);
			Graphics()->TextureClear();
			IGraphics::CLineItemBatch LineItemBatch;
			for(int c = 0; c < pEnvelope->GetChannels(); c++)
			{
				Graphics()->LinesBatchBegin(&LineItemBatch);
				if(s_ActiveChannels & (1 << c))
					Graphics()->SetColor(aColors[c].r, aColors[c].g, aColors[c].b, 1);
				else
					Graphics()->SetColor(aColors[c].r * 0.5f, aColors[c].g * 0.5f, aColors[c].b * 0.5f, 1);

				const int Steps = static_cast<int>(((EndX - StartX) / Ui()->Screen()->w) * Graphics()->ScreenWidth());
				const float StepTime = (EndTime - StartTime) / static_cast<float>(Steps);
				const float StepSize = (EndX - StartX) / static_cast<float>(Steps);

				ColorRGBA Channels = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
				pEnvelope->Eval(StartTime, Channels, c + 1);
				float PrevTime = StartTime;
				float PrevX = StartX;
				float PrevY = EnvelopeToScreenY(View, Channels[c]);
				for(int Step = 1; Step <= Steps; Step++)
				{
					float CurrentTime = StartTime + Step * StepTime;
					if(CurrentTime >= EndTime)
					{
						CurrentTime = EndTime - 0.001f;
						if(CurrentTime <= PrevTime)
							break;
					}

					Channels = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
					pEnvelope->Eval(CurrentTime, Channels, c + 1);
					const float CurrentX = StartX + Step * StepSize;
					const float CurrentY = EnvelopeToScreenY(View, Channels[c]);

					const IGraphics::CLineItem Item = IGraphics::CLineItem(PrevX, PrevY, CurrentX, CurrentY);
					Graphics()->LinesBatchDraw(&LineItemBatch, &Item, 1);

					PrevTime = CurrentTime;
					PrevX = CurrentX;
					PrevY = CurrentY;
				}
				Graphics()->LinesBatchEnd(&LineItemBatch);
			}
			Ui()->ClipDisable();
		}

		// render curve options
		{
			for(int i = 0; i < (int)pEnvelope->m_vPoints.size() - 1; i++)
			{
				float t0 = pEnvelope->m_vPoints[i].m_Time.AsSeconds();
				float t1 = pEnvelope->m_vPoints[i + 1].m_Time.AsSeconds();

				CUIRect CurveButton;
				CurveButton.x = EnvelopeToScreenX(View, t0 + (t1 - t0) * 0.5f);
				CurveButton.y = CurveBar.y;
				CurveButton.h = CurveBar.h;
				CurveButton.w = CurveBar.h;
				CurveButton.x -= CurveButton.w / 2.0f;
				const void *pId = &pEnvelope->m_vPoints[i].m_Curvetype;
				static const char *const TYPE_NAMES[NUM_CURVETYPES] = {"N", "L", "S", "F", "M", "B"};
				const char *pTypeName = "!?";
				if(0 <= pEnvelope->m_vPoints[i].m_Curvetype && pEnvelope->m_vPoints[i].m_Curvetype < (int)std::size(TYPE_NAMES))
					pTypeName = TYPE_NAMES[pEnvelope->m_vPoints[i].m_Curvetype];

				if(CurveButton.x >= View.x)
				{
					const int ButtonResult = DoButton_Editor(pId, pTypeName, 0, &CurveButton, BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT, "Switch curve type (N = step, L = linear, S = slow, F = fast, M = smooth, B = bezier).");
					if(ButtonResult == 1)
					{
						const int PrevCurve = pEnvelope->m_vPoints[i].m_Curvetype;
						const int Direction = Input()->ShiftIsPressed() ? -1 : 1;
						pEnvelope->m_vPoints[i].m_Curvetype = (pEnvelope->m_vPoints[i].m_Curvetype + Direction + NUM_CURVETYPES) % NUM_CURVETYPES;

						Map()->m_EnvelopeEditorHistory.RecordAction(std::make_shared<CEditorActionEnvelopeEditPoint>(Map(),
							Map()->m_SelectedEnvelope, i, 0, CEditorActionEnvelopeEditPoint::EEditType::CURVE_TYPE, PrevCurve, pEnvelope->m_vPoints[i].m_Curvetype));
						Map()->OnModify();
					}
					else if(ButtonResult == 2)
					{
						m_PopupEnvelopeSelectedPoint = i;
						static SPopupMenuId s_PopupCurvetypeId;
						Ui()->DoPopupMenu(&s_PopupCurvetypeId, Ui()->MouseX(), Ui()->MouseY(), 80, (float)NUM_CURVETYPES * 14.0f + 10.0f, this, PopupEnvelopeCurvetype);
					}
				}
			}
		}

		// render colorbar
		if(ShowColorBar)
		{
			RenderEnvelopeEditorColorBar(ColorBar, pEnvelope);
		}

		// render handles
		if(CurrentEnvelopeSwitched)
		{
			Map()->DeselectEnvPoints();
			m_ResetZoomEnvelope = true;
		}

		{
			static SPopupMenuId s_PopupEnvPointId;
			const auto &&ShowPopupEnvPoint = [&]() {
				Ui()->DoPopupMenu(&s_PopupEnvPointId, Ui()->MouseX(), Ui()->MouseY(), 150, 56 + (pEnvelope->GetChannels() == 4 && !Map()->IsTangentSelected() ? 16.0f : 0.0f), this, PopupEnvPoint);
			};

			if(s_Operation == EEnvelopeEditorOp::NONE)
			{
				UpdateHotEnvelopePoint(View, pEnvelope.get(), s_ActiveChannels);
				if(!Ui()->MouseButton(0))
					Map()->m_EnvOpTracker.Stop(false);
			}
			else
			{
				Map()->m_EnvOpTracker.Begin(s_Operation);
			}

			Ui()->ClipEnable(&View);
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			for(int c = 0; c < pEnvelope->GetChannels(); c++)
			{
				if(!(s_ActiveChannels & (1 << c)))
					continue;

				for(int i = 0; i < (int)pEnvelope->m_vPoints.size(); i++)
				{
					// point handle
					{
						CUIRect Final;
						Final.x = EnvelopeToScreenX(View, pEnvelope->m_vPoints[i].m_Time.AsSeconds());
						Final.y = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c]));
						Final.x -= 2.0f;
						Final.y -= 2.0f;
						Final.w = 4.0f;
						Final.h = 4.0f;

						const void *pId = &pEnvelope->m_vPoints[i].m_aValues[c];

						if(Map()->IsEnvPointSelected(i, c))
						{
							Graphics()->SetColor(1, 1, 1, 1);
							CUIRect Background = {
								Final.x - 0.2f * Final.w,
								Final.y - 0.2f * Final.h,
								Final.w * 1.4f,
								Final.h * 1.4f};
							IGraphics::CQuadItem QuadItem(Background.x, Background.y, Background.w, Background.h);
							Graphics()->QuadsDrawTL(&QuadItem, 1);
						}

						if(Ui()->CheckActiveItem(pId))
						{
							m_ActiveEnvelopePreview = EEnvelopePreview::SELECTED;

							if(s_Operation == EEnvelopeEditorOp::SELECT)
							{
								float dx = s_MouseXStart - Ui()->MouseX();
								float dy = s_MouseYStart - Ui()->MouseY();

								if(dx * dx + dy * dy > 20.0f)
								{
									s_Operation = EEnvelopeEditorOp::DRAG_POINT;

									if(!Map()->IsEnvPointSelected(i, c))
										Map()->SelectEnvPoint(i, c);
								}
							}

							if(s_Operation == EEnvelopeEditorOp::DRAG_POINT || s_Operation == EEnvelopeEditorOp::DRAG_POINT_X || s_Operation == EEnvelopeEditorOp::DRAG_POINT_Y)
							{
								if(Input()->ShiftIsPressed())
								{
									if(s_Operation == EEnvelopeEditorOp::DRAG_POINT || s_Operation == EEnvelopeEditorOp::DRAG_POINT_Y)
									{
										s_Operation = EEnvelopeEditorOp::DRAG_POINT_X;
										s_vAccurateDragValuesX.clear();
										for(auto [SelectedIndex, _] : Map()->m_vSelectedEnvelopePoints)
											s_vAccurateDragValuesX.push_back(pEnvelope->m_vPoints[SelectedIndex].m_Time.GetInternal());
									}
									else
									{
										float DeltaX = ScreenToEnvelopeDX(View, Ui()->MouseDeltaX()) * (Input()->ModifierIsPressed() ? 50.0f : 1000.0f);

										for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
										{
											int SelectedIndex = Map()->m_vSelectedEnvelopePoints[k].first;
											CFixedTime BoundLow = CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x));
											CFixedTime BoundHigh = CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x + View.w));
											for(int j = 0; j < SelectedIndex; j++)
											{
												if(!Map()->IsEnvPointSelected(j))
													BoundLow = std::max(pEnvelope->m_vPoints[j].m_Time + CFixedTime(1), BoundLow);
											}
											for(int j = SelectedIndex + 1; j < (int)pEnvelope->m_vPoints.size(); j++)
											{
												if(!Map()->IsEnvPointSelected(j))
													BoundHigh = std::min(pEnvelope->m_vPoints[j].m_Time - CFixedTime(1), BoundHigh);
											}

											DeltaX = ClampDelta(s_vAccurateDragValuesX[k], DeltaX, BoundLow.GetInternal(), BoundHigh.GetInternal());
										}
										for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
										{
											int SelectedIndex = Map()->m_vSelectedEnvelopePoints[k].first;
											s_vAccurateDragValuesX[k] += DeltaX;
											pEnvelope->m_vPoints[SelectedIndex].m_Time = CFixedTime(std::round(s_vAccurateDragValuesX[k]));
										}
										for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
										{
											int SelectedIndex = Map()->m_vSelectedEnvelopePoints[k].first;
											if(SelectedIndex == 0 && pEnvelope->m_vPoints[SelectedIndex].m_Time != CFixedTime(0))
											{
												RemoveTimeOffsetEnvelope(pEnvelope);
												float Offset = s_vAccurateDragValuesX[k];
												for(auto &Value : s_vAccurateDragValuesX)
													Value -= Offset;
												break;
											}
										}
									}
								}
								else
								{
									if(s_Operation == EEnvelopeEditorOp::DRAG_POINT || s_Operation == EEnvelopeEditorOp::DRAG_POINT_X)
									{
										s_Operation = EEnvelopeEditorOp::DRAG_POINT_Y;
										s_vAccurateDragValuesY.clear();
										for(auto [SelectedIndex, SelectedChannel] : Map()->m_vSelectedEnvelopePoints)
											s_vAccurateDragValuesY.push_back(pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel]);
									}
									else
									{
										float DeltaY = ScreenToEnvelopeDY(View, Ui()->MouseDeltaY()) * (Input()->ModifierIsPressed() ? 51.2f : 1024.0f);
										for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
										{
											auto [SelectedIndex, SelectedChannel] = Map()->m_vSelectedEnvelopePoints[k];
											s_vAccurateDragValuesY[k] -= DeltaY;
											pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel] = std::round(s_vAccurateDragValuesY[k]);

											if(pEnvelope->GetChannels() == 1 || pEnvelope->GetChannels() == 4)
											{
												pEnvelope->m_vPoints[i].m_aValues[c] = std::clamp(pEnvelope->m_vPoints[i].m_aValues[c], 0, 1024);
												s_vAccurateDragValuesY[k] = std::clamp<float>(s_vAccurateDragValuesY[k], 0, 1024);
											}
										}
									}
								}
							}

							if(s_Operation == EEnvelopeEditorOp::CONTEXT_MENU)
							{
								if(!Ui()->MouseButton(1))
								{
									if(Map()->m_vSelectedEnvelopePoints.size() == 1)
									{
										Map()->m_UpdateEnvPointInfo = true;
										ShowPopupEnvPoint();
									}
									else if(Map()->m_vSelectedEnvelopePoints.size() > 1)
									{
										static SPopupMenuId s_PopupEnvPointMultiId;
										Ui()->DoPopupMenu(&s_PopupEnvPointMultiId, Ui()->MouseX(), Ui()->MouseY(), 80, 22, this, PopupEnvPointMulti);
									}
									Ui()->SetActiveItem(nullptr);
									s_Operation = EEnvelopeEditorOp::NONE;
								}
							}
							else if(!Ui()->MouseButton(0))
							{
								Ui()->SetActiveItem(nullptr);
								Map()->m_SelectedQuadEnvelope = -1;

								if(s_Operation == EEnvelopeEditorOp::SELECT)
								{
									if(Input()->ShiftIsPressed())
										Map()->ToggleEnvPoint(i, c);
									else
										Map()->SelectEnvPoint(i, c);
								}

								s_Operation = EEnvelopeEditorOp::NONE;
								Map()->OnModify();
							}

							Graphics()->SetColor(1, 1, 1, 1);
						}
						else if(Ui()->HotItem() == pId)
						{
							if(Ui()->MouseButton(0))
							{
								Ui()->SetActiveItem(pId);
								s_Operation = EEnvelopeEditorOp::SELECT;
								Map()->m_SelectedQuadEnvelope = Map()->m_SelectedEnvelope;

								s_MouseXStart = Ui()->MouseX();
								s_MouseYStart = Ui()->MouseY();
							}
							else if(Ui()->MouseButtonClicked(1))
							{
								if(Input()->ShiftIsPressed())
								{
									Map()->m_EnvelopeEditorHistory.Execute(std::make_shared<CEditorActionDeleteEnvelopePoint>(Map(), Map()->m_SelectedEnvelope, i));
								}
								else
								{
									s_Operation = EEnvelopeEditorOp::CONTEXT_MENU;
									if(!Map()->IsEnvPointSelected(i, c))
										Map()->SelectEnvPoint(i, c);
									Ui()->SetActiveItem(pId);
								}
							}

							m_ActiveEnvelopePreview = EEnvelopePreview::SELECTED;
							Graphics()->SetColor(1, 1, 1, 1);
							str_copy(m_aTooltip, "Envelope point. Left mouse to drag. Hold ctrl to be more precise. Hold shift to alter time. Shift+right click to delete.");
							m_pUiGotContext = pId;
						}
						else
							Graphics()->SetColor(aColors[c].r, aColors[c].g, aColors[c].b, 1.0f);

						IGraphics::CQuadItem QuadItem(Final.x, Final.y, Final.w, Final.h);
						Graphics()->QuadsDrawTL(&QuadItem, 1);
					}

					// tangent handles for bezier curves
					if(i >= 0 && i < (int)pEnvelope->m_vPoints.size())
					{
						// Out-Tangent handle
						if(i < (int)pEnvelope->m_vPoints.size() - 1 && pEnvelope->m_vPoints[i].m_Curvetype == CURVETYPE_BEZIER)
						{
							CUIRect Final;
							Final.x = EnvelopeToScreenX(View, (pEnvelope->m_vPoints[i].m_Time + pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c]).AsSeconds());
							Final.y = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c] + pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaY[c]));
							Final.x -= 2.0f;
							Final.y -= 2.0f;
							Final.w = 4.0f;
							Final.h = 4.0f;

							// handle logic
							const void *pId = &pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c];

							if(Map()->IsTangentOutPointSelected(i, c))
							{
								Graphics()->SetColor(1, 1, 1, 1);
								IGraphics::CFreeformItem FreeformItem(
									Final.x + Final.w / 2.0f,
									Final.y - 1,
									Final.x + Final.w / 2.0f,
									Final.y - 1,
									Final.x + Final.w + 1,
									Final.y + Final.h + 1,
									Final.x - 1,
									Final.y + Final.h + 1);
								Graphics()->QuadsDrawFreeform(&FreeformItem, 1);
							}

							if(Ui()->CheckActiveItem(pId))
							{
								m_ActiveEnvelopePreview = EEnvelopePreview::SELECTED;

								if(s_Operation == EEnvelopeEditorOp::SELECT)
								{
									float dx = s_MouseXStart - Ui()->MouseX();
									float dy = s_MouseYStart - Ui()->MouseY();

									if(dx * dx + dy * dy > 20.0f)
									{
										s_Operation = EEnvelopeEditorOp::DRAG_POINT;

										s_vAccurateDragValuesX = {static_cast<float>(pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c].GetInternal())};
										s_vAccurateDragValuesY = {static_cast<float>(pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaY[c])};

										if(!Map()->IsTangentOutPointSelected(i, c))
											Map()->SelectTangentOutPoint(i, c);
									}
								}

								if(s_Operation == EEnvelopeEditorOp::DRAG_POINT)
								{
									float DeltaX = ScreenToEnvelopeDX(View, Ui()->MouseDeltaX()) * (Input()->ModifierIsPressed() ? 50.0f : 1000.0f);
									float DeltaY = ScreenToEnvelopeDY(View, Ui()->MouseDeltaY()) * (Input()->ModifierIsPressed() ? 51.2f : 1024.0f);
									s_vAccurateDragValuesX[0] += DeltaX;
									s_vAccurateDragValuesY[0] -= DeltaY;

									pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c] = CFixedTime(std::round(s_vAccurateDragValuesX[0]));
									pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaY[c] = std::round(s_vAccurateDragValuesY[0]);

									// clamp time value
									pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c] = std::clamp(pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c], CFixedTime(0), CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x + View.w)) - pEnvelope->m_vPoints[i].m_Time);
									s_vAccurateDragValuesX[0] = std::clamp<float>(s_vAccurateDragValuesX[0], 0, (CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x + View.w)) - pEnvelope->m_vPoints[i].m_Time).GetInternal());
								}

								if(s_Operation == EEnvelopeEditorOp::CONTEXT_MENU)
								{
									if(!Ui()->MouseButton(1))
									{
										if(Map()->IsTangentOutPointSelected(i, c))
										{
											Map()->m_UpdateEnvPointInfo = true;
											ShowPopupEnvPoint();
										}
										Ui()->SetActiveItem(nullptr);
										s_Operation = EEnvelopeEditorOp::NONE;
									}
								}
								else if(!Ui()->MouseButton(0))
								{
									Ui()->SetActiveItem(nullptr);
									Map()->m_SelectedQuadEnvelope = -1;

									if(s_Operation == EEnvelopeEditorOp::SELECT)
										Map()->SelectTangentOutPoint(i, c);

									s_Operation = EEnvelopeEditorOp::NONE;
									Map()->OnModify();
								}

								Graphics()->SetColor(1, 1, 1, 1);
							}
							else if(Ui()->HotItem() == pId)
							{
								if(Ui()->MouseButton(0))
								{
									Ui()->SetActiveItem(pId);
									s_Operation = EEnvelopeEditorOp::SELECT;
									Map()->m_SelectedQuadEnvelope = Map()->m_SelectedEnvelope;

									s_MouseXStart = Ui()->MouseX();
									s_MouseYStart = Ui()->MouseY();
								}
								else if(Ui()->MouseButtonClicked(1))
								{
									if(Input()->ShiftIsPressed())
									{
										Map()->SelectTangentOutPoint(i, c);
										pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaX[c] = CFixedTime(0);
										pEnvelope->m_vPoints[i].m_Bezier.m_aOutTangentDeltaY[c] = 0.0f;
										Map()->OnModify();
									}
									else
									{
										s_Operation = EEnvelopeEditorOp::CONTEXT_MENU;
										Map()->SelectTangentOutPoint(i, c);
										Ui()->SetActiveItem(pId);
									}
								}

								m_ActiveEnvelopePreview = EEnvelopePreview::SELECTED;
								Graphics()->SetColor(1, 1, 1, 1);
								str_copy(m_aTooltip, "Bezier out-tangent. Left mouse to drag. Hold ctrl to be more precise. Shift+right click to reset.");
								m_pUiGotContext = pId;
							}
							else
								Graphics()->SetColor(aColors[c].r, aColors[c].g, aColors[c].b, 1.0f);

							// draw triangle
							IGraphics::CFreeformItem FreeformItem(Final.x + Final.w / 2.0f, Final.y, Final.x + Final.w / 2.0f, Final.y, Final.x + Final.w, Final.y + Final.h, Final.x, Final.y + Final.h);
							Graphics()->QuadsDrawFreeform(&FreeformItem, 1);
						}

						// In-Tangent handle
						if(i > 0 && pEnvelope->m_vPoints[i - 1].m_Curvetype == CURVETYPE_BEZIER)
						{
							CUIRect Final;
							Final.x = EnvelopeToScreenX(View, (pEnvelope->m_vPoints[i].m_Time + pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c]).AsSeconds());
							Final.y = EnvelopeToScreenY(View, fx2f(pEnvelope->m_vPoints[i].m_aValues[c] + pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaY[c]));
							Final.x -= 2.0f;
							Final.y -= 2.0f;
							Final.w = 4.0f;
							Final.h = 4.0f;

							// handle logic
							const void *pId = &pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c];

							if(Map()->IsTangentInPointSelected(i, c))
							{
								Graphics()->SetColor(1, 1, 1, 1);
								IGraphics::CFreeformItem FreeformItem(
									Final.x + Final.w / 2.0f,
									Final.y - 1,
									Final.x + Final.w / 2.0f,
									Final.y - 1,
									Final.x + Final.w + 1,
									Final.y + Final.h + 1,
									Final.x - 1,
									Final.y + Final.h + 1);
								Graphics()->QuadsDrawFreeform(&FreeformItem, 1);
							}

							if(Ui()->CheckActiveItem(pId))
							{
								m_ActiveEnvelopePreview = EEnvelopePreview::SELECTED;

								if(s_Operation == EEnvelopeEditorOp::SELECT)
								{
									float dx = s_MouseXStart - Ui()->MouseX();
									float dy = s_MouseYStart - Ui()->MouseY();

									if(dx * dx + dy * dy > 20.0f)
									{
										s_Operation = EEnvelopeEditorOp::DRAG_POINT;

										s_vAccurateDragValuesX = {static_cast<float>(pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c].GetInternal())};
										s_vAccurateDragValuesY = {static_cast<float>(pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaY[c])};

										if(!Map()->IsTangentInPointSelected(i, c))
											Map()->SelectTangentInPoint(i, c);
									}
								}

								if(s_Operation == EEnvelopeEditorOp::DRAG_POINT)
								{
									float DeltaX = ScreenToEnvelopeDX(View, Ui()->MouseDeltaX()) * (Input()->ModifierIsPressed() ? 50.0f : 1000.0f);
									float DeltaY = ScreenToEnvelopeDY(View, Ui()->MouseDeltaY()) * (Input()->ModifierIsPressed() ? 51.2f : 1024.0f);
									s_vAccurateDragValuesX[0] += DeltaX;
									s_vAccurateDragValuesY[0] -= DeltaY;

									pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c] = CFixedTime(std::round(s_vAccurateDragValuesX[0]));
									pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaY[c] = std::round(s_vAccurateDragValuesY[0]);

									// clamp time value
									pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c] = std::clamp(pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c], CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x)) - pEnvelope->m_vPoints[i].m_Time, CFixedTime(0));
									s_vAccurateDragValuesX[0] = std::clamp<float>(s_vAccurateDragValuesX[0], (CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x)) - pEnvelope->m_vPoints[i].m_Time).GetInternal(), 0);
								}

								if(s_Operation == EEnvelopeEditorOp::CONTEXT_MENU)
								{
									if(!Ui()->MouseButton(1))
									{
										if(Map()->IsTangentInPointSelected(i, c))
										{
											Map()->m_UpdateEnvPointInfo = true;
											ShowPopupEnvPoint();
										}
										Ui()->SetActiveItem(nullptr);
										s_Operation = EEnvelopeEditorOp::NONE;
									}
								}
								else if(!Ui()->MouseButton(0))
								{
									Ui()->SetActiveItem(nullptr);
									Map()->m_SelectedQuadEnvelope = -1;

									if(s_Operation == EEnvelopeEditorOp::SELECT)
										Map()->SelectTangentInPoint(i, c);

									s_Operation = EEnvelopeEditorOp::NONE;
									Map()->OnModify();
								}

								Graphics()->SetColor(1, 1, 1, 1);
							}
							else if(Ui()->HotItem() == pId)
							{
								if(Ui()->MouseButton(0))
								{
									Ui()->SetActiveItem(pId);
									s_Operation = EEnvelopeEditorOp::SELECT;
									Map()->m_SelectedQuadEnvelope = Map()->m_SelectedEnvelope;

									s_MouseXStart = Ui()->MouseX();
									s_MouseYStart = Ui()->MouseY();
								}
								else if(Ui()->MouseButtonClicked(1))
								{
									if(Input()->ShiftIsPressed())
									{
										Map()->SelectTangentInPoint(i, c);
										pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaX[c] = CFixedTime(0);
										pEnvelope->m_vPoints[i].m_Bezier.m_aInTangentDeltaY[c] = 0.0f;
										Map()->OnModify();
									}
									else
									{
										s_Operation = EEnvelopeEditorOp::CONTEXT_MENU;
										Map()->SelectTangentInPoint(i, c);
										Ui()->SetActiveItem(pId);
									}
								}

								m_ActiveEnvelopePreview = EEnvelopePreview::SELECTED;
								Graphics()->SetColor(1, 1, 1, 1);
								str_copy(m_aTooltip, "Bezier in-tangent. Left mouse to drag. Hold ctrl to be more precise. Shift+right click to reset.");
								m_pUiGotContext = pId;
							}
							else
								Graphics()->SetColor(aColors[c].r, aColors[c].g, aColors[c].b, 1.0f);

							// draw triangle
							IGraphics::CFreeformItem FreeformItem(Final.x + Final.w / 2.0f, Final.y, Final.x + Final.w / 2.0f, Final.y, Final.x + Final.w, Final.y + Final.h, Final.x, Final.y + Final.h);
							Graphics()->QuadsDrawFreeform(&FreeformItem, 1);
						}
					}
				}
			}
			Graphics()->QuadsEnd();
			Ui()->ClipDisable();
		}

		// handle scaling
		static float s_ScaleFactorX = 1.0f;
		static float s_ScaleFactorY = 1.0f;
		static float s_MidpointX = 0.0f;
		static float s_MidpointY = 0.0f;
		static std::vector<float> s_vInitialPositionsX;
		static std::vector<float> s_vInitialPositionsY;
		if(s_Operation == EEnvelopeEditorOp::NONE && !s_NameInput.IsActive() && Input()->KeyIsPressed(KEY_S) && !Input()->ModifierIsPressed() && !Map()->m_vSelectedEnvelopePoints.empty())
		{
			s_Operation = EEnvelopeEditorOp::SCALE;
			s_ScaleFactorX = 1.0f;
			s_ScaleFactorY = 1.0f;
			auto [FirstPointIndex, FirstPointChannel] = Map()->m_vSelectedEnvelopePoints.front();

			float MaximumX = pEnvelope->m_vPoints[FirstPointIndex].m_Time.GetInternal();
			float MinimumX = MaximumX;
			s_vInitialPositionsX.clear();
			for(auto [SelectedIndex, _] : Map()->m_vSelectedEnvelopePoints)
			{
				float Value = pEnvelope->m_vPoints[SelectedIndex].m_Time.GetInternal();
				s_vInitialPositionsX.push_back(Value);
				MaximumX = maximum(MaximumX, Value);
				MinimumX = minimum(MinimumX, Value);
			}
			s_MidpointX = (MaximumX - MinimumX) / 2.0f + MinimumX;

			float MaximumY = pEnvelope->m_vPoints[FirstPointIndex].m_aValues[FirstPointChannel];
			float MinimumY = MaximumY;
			s_vInitialPositionsY.clear();
			for(auto [SelectedIndex, SelectedChannel] : Map()->m_vSelectedEnvelopePoints)
			{
				float Value = pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel];
				s_vInitialPositionsY.push_back(Value);
				MaximumY = maximum(MaximumY, Value);
				MinimumY = minimum(MinimumY, Value);
			}
			s_MidpointY = (MaximumY - MinimumY) / 2.0f + MinimumY;
		}

		if(s_Operation == EEnvelopeEditorOp::SCALE)
		{
			str_copy(m_aTooltip, "Press shift to scale the time. Press alt to scale along midpoint. Press ctrl to be more precise.");

			if(Input()->ShiftIsPressed())
			{
				s_ScaleFactorX += Ui()->MouseDeltaX() / Graphics()->ScreenWidth() * (Input()->ModifierIsPressed() ? 0.5f : 10.0f);
				float Midpoint = Input()->AltIsPressed() ? s_MidpointX : 0.0f;
				for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
				{
					int SelectedIndex = Map()->m_vSelectedEnvelopePoints[k].first;
					CFixedTime BoundLow = CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x));
					CFixedTime BoundHigh = CFixedTime::FromSeconds(ScreenToEnvelopeX(View, View.x + View.w));
					for(int j = 0; j < SelectedIndex; j++)
					{
						if(!Map()->IsEnvPointSelected(j))
							BoundLow = std::max(pEnvelope->m_vPoints[j].m_Time + CFixedTime(1), BoundLow);
					}
					for(int j = SelectedIndex + 1; j < (int)pEnvelope->m_vPoints.size(); j++)
					{
						if(!Map()->IsEnvPointSelected(j))
							BoundHigh = std::min(pEnvelope->m_vPoints[j].m_Time - CFixedTime(1), BoundHigh);
					}

					float Value = s_vInitialPositionsX[k];
					float ScaleBoundLow = (BoundLow.GetInternal() - Midpoint) / (Value - Midpoint);
					float ScaleBoundHigh = (BoundHigh.GetInternal() - Midpoint) / (Value - Midpoint);
					float ScaleBoundMin = minimum(ScaleBoundLow, ScaleBoundHigh);
					float ScaleBoundMax = maximum(ScaleBoundLow, ScaleBoundHigh);
					s_ScaleFactorX = std::clamp(s_ScaleFactorX, ScaleBoundMin, ScaleBoundMax);
				}

				for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
				{
					int SelectedIndex = Map()->m_vSelectedEnvelopePoints[k].first;
					float ScaleMinimum = s_vInitialPositionsX[k] - Midpoint > CFixedTime(1).AsSeconds() ? CFixedTime(1).AsSeconds() / (s_vInitialPositionsX[k] - Midpoint) : 0.0f;
					float ScaleFactor = maximum(ScaleMinimum, s_ScaleFactorX);
					pEnvelope->m_vPoints[SelectedIndex].m_Time = CFixedTime(std::round((s_vInitialPositionsX[k] - Midpoint) * ScaleFactor + Midpoint));
				}
				for(size_t k = 1; k < pEnvelope->m_vPoints.size(); k++)
				{
					if(pEnvelope->m_vPoints[k].m_Time <= pEnvelope->m_vPoints[k - 1].m_Time)
						pEnvelope->m_vPoints[k].m_Time = pEnvelope->m_vPoints[k - 1].m_Time + CFixedTime(1);
				}
				for(auto [SelectedIndex, _] : Map()->m_vSelectedEnvelopePoints)
				{
					if(SelectedIndex == 0 && pEnvelope->m_vPoints[SelectedIndex].m_Time != CFixedTime(0))
					{
						float Offset = pEnvelope->m_vPoints[0].m_Time.GetInternal();
						RemoveTimeOffsetEnvelope(pEnvelope);
						s_MidpointX -= Offset;
						for(auto &Value : s_vInitialPositionsX)
							Value -= Offset;
						break;
					}
				}
			}
			else
			{
				s_ScaleFactorY -= Ui()->MouseDeltaY() / Graphics()->ScreenHeight() * (Input()->ModifierIsPressed() ? 0.5f : 10.0f);
				for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
				{
					auto [SelectedIndex, SelectedChannel] = Map()->m_vSelectedEnvelopePoints[k];
					if(Input()->AltIsPressed())
						pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel] = std::round((s_vInitialPositionsY[k] - s_MidpointY) * s_ScaleFactorY + s_MidpointY);
					else
						pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel] = std::round(s_vInitialPositionsY[k] * s_ScaleFactorY);

					if(pEnvelope->GetChannels() == 1 || pEnvelope->GetChannels() == 4)
						pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel] = std::clamp(pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel], 0, 1024);
				}
			}

			if(Ui()->MouseButton(0))
			{
				s_Operation = EEnvelopeEditorOp::NONE;
				Map()->m_EnvOpTracker.Stop(false);
			}
			else if(Ui()->MouseButton(1) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			{
				for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
				{
					int SelectedIndex = Map()->m_vSelectedEnvelopePoints[k].first;
					pEnvelope->m_vPoints[SelectedIndex].m_Time = CFixedTime(std::round(s_vInitialPositionsX[k]));
				}
				for(size_t k = 0; k < Map()->m_vSelectedEnvelopePoints.size(); k++)
				{
					auto [SelectedIndex, SelectedChannel] = Map()->m_vSelectedEnvelopePoints[k];
					pEnvelope->m_vPoints[SelectedIndex].m_aValues[SelectedChannel] = std::round(s_vInitialPositionsY[k]);
				}
				RemoveTimeOffsetEnvelope(pEnvelope);
				s_Operation = EEnvelopeEditorOp::NONE;
			}
		}

		// handle box selection
		if(s_Operation == EEnvelopeEditorOp::BOX_SELECT)
		{
			Ui()->ClipEnable(&View);
			CUIRect SelectionRect;
			SelectionRect.x = s_MouseXStart;
			SelectionRect.y = s_MouseYStart;
			SelectionRect.w = Ui()->MouseX() - s_MouseXStart;
			SelectionRect.h = Ui()->MouseY() - s_MouseYStart;
			SelectionRect.DrawOutline(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
			Ui()->ClipDisable();

			if(!Ui()->MouseButton(0))
			{
				s_Operation = EEnvelopeEditorOp::NONE;
				Ui()->SetActiveItem(nullptr);

				float TimeStart = ScreenToEnvelopeX(View, s_MouseXStart);
				float TimeEnd = ScreenToEnvelopeX(View, Ui()->MouseX());
				float ValueStart = ScreenToEnvelopeY(View, s_MouseYStart);
				float ValueEnd = ScreenToEnvelopeY(View, Ui()->MouseY());

				float TimeMin = minimum(TimeStart, TimeEnd);
				float TimeMax = maximum(TimeStart, TimeEnd);
				float ValueMin = minimum(ValueStart, ValueEnd);
				float ValueMax = maximum(ValueStart, ValueEnd);

				if(!Input()->ShiftIsPressed())
					Map()->DeselectEnvPoints();

				for(int i = 0; i < (int)pEnvelope->m_vPoints.size(); i++)
				{
					for(int c = 0; c < CEnvPoint::MAX_CHANNELS; c++)
					{
						if(!(s_ActiveChannels & (1 << c)))
							continue;

						float Time = pEnvelope->m_vPoints[i].m_Time.AsSeconds();
						float Value = fx2f(pEnvelope->m_vPoints[i].m_aValues[c]);

						if(in_range(Time, TimeMin, TimeMax) && in_range(Value, ValueMin, ValueMax))
							Map()->ToggleEnvPoint(i, c);
					}
				}
			}
		}
	}
}

void CEditor::RenderEnvelopeEditorColorBar(CUIRect ColorBar, const std::shared_ptr<CEnvelope> &pEnvelope)
{
	if(pEnvelope->m_vPoints.size() < 2)
	{
		return;
	}
	const float ViewStartTime = ScreenToEnvelopeX(ColorBar, ColorBar.x);
	const float ViewEndTime = ScreenToEnvelopeX(ColorBar, ColorBar.x + ColorBar.w);
	if(ViewEndTime < 0.0f || ViewStartTime > pEnvelope->EndTime())
	{
		return;
	}
	const float StartX = maximum(EnvelopeToScreenX(ColorBar, 0.0f), ColorBar.x);
	const float TotalWidth = minimum(EnvelopeToScreenX(ColorBar, pEnvelope->EndTime()) - StartX, ColorBar.x + ColorBar.w - StartX);

	Ui()->ClipEnable(&ColorBar);
	CUIRect ColorBarBackground = CUIRect{StartX, ColorBar.y, TotalWidth, ColorBar.h};
	RenderBackground(ColorBarBackground, m_CheckerTexture, ColorBarBackground.h, 1.0f);
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();

	int PointBeginIndex = pEnvelope->FindPointIndex(CFixedTime::FromSeconds(ViewStartTime));
	if(PointBeginIndex == -1)
	{
		PointBeginIndex = 0;
	}
	int PointEndIndex = pEnvelope->FindPointIndex(CFixedTime::FromSeconds(ViewEndTime));
	if(PointEndIndex == -1)
	{
		PointEndIndex = (int)pEnvelope->m_vPoints.size() - 2;
	}
	for(int PointIndex = PointBeginIndex; PointIndex <= PointEndIndex; PointIndex++)
	{
		const auto &PointStart = pEnvelope->m_vPoints[PointIndex];
		const auto &PointEnd = pEnvelope->m_vPoints[PointIndex + 1];
		const float PointStartTime = PointStart.m_Time.AsSeconds();
		const float PointEndTime = PointEnd.m_Time.AsSeconds();

		int Steps;
		if(PointStart.m_Curvetype == CURVETYPE_LINEAR || PointStart.m_Curvetype == CURVETYPE_STEP)
		{
			Steps = 1; // let the GPU do the work
		}
		else
		{
			const float ClampedPointStartX = maximum(EnvelopeToScreenX(ColorBar, PointStartTime), ColorBar.x);
			const float ClampedPointEndX = minimum(EnvelopeToScreenX(ColorBar, PointEndTime), ColorBar.x + ColorBar.w);
			Steps = std::clamp((int)std::sqrt(5.0f * (ClampedPointEndX - ClampedPointStartX)), 1, 250);
		}
		const float OverallSectionStartTime = Steps == 1 ? PointStartTime : maximum(PointStartTime, ViewStartTime);
		const float OverallSectionEndTime = Steps == 1 ? PointEndTime : minimum(PointEndTime, ViewEndTime);
		float SectionStartTime = OverallSectionStartTime;
		float SectionStartX = EnvelopeToScreenX(ColorBar, SectionStartTime);
		for(int Step = 1; Step <= Steps; Step++)
		{
			const float SectionEndTime = OverallSectionStartTime + (OverallSectionEndTime - OverallSectionStartTime) * (Step / (float)Steps);
			const float SectionEndX = EnvelopeToScreenX(ColorBar, SectionEndTime);

			ColorRGBA StartColor;
			if(Step == 1 && OverallSectionStartTime == PointStartTime)
			{
				StartColor = PointStart.ColorValue();
			}
			else
			{
				StartColor = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
				pEnvelope->Eval(SectionStartTime, StartColor, 4);
			}

			ColorRGBA EndColor;
			if(PointStart.m_Curvetype == CURVETYPE_STEP)
			{
				EndColor = StartColor;
			}
			else if(Step == Steps && OverallSectionEndTime == PointEndTime)
			{
				EndColor = PointEnd.ColorValue();
			}
			else
			{
				EndColor = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
				pEnvelope->Eval(SectionEndTime, EndColor, 4);
			}

			Graphics()->SetColor4(StartColor, EndColor, StartColor, EndColor);
			const IGraphics::CQuadItem QuadItem(SectionStartX, ColorBar.y, SectionEndX - SectionStartX, ColorBar.h);
			Graphics()->QuadsDrawTL(&QuadItem, 1);

			SectionStartTime = SectionEndTime;
			SectionStartX = SectionEndX;
		}
	}
	Graphics()->QuadsEnd();
	Ui()->ClipDisable();
	ColorBarBackground.h -= Ui()->Screen()->h / Graphics()->ScreenHeight(); // hack to fix alignment of bottom border
	ColorBarBackground.DrawOutline(ColorRGBA(0.7f, 0.7f, 0.7f, 1.0f));
}

void CEditor::RenderEditorHistory(CUIRect View)
{
	enum EHistoryType
	{
		EDITOR_HISTORY,
		ENVELOPE_HISTORY,
		SERVER_SETTINGS_HISTORY
	};

	static EHistoryType s_HistoryType = EDITOR_HISTORY;
	static int s_ActionSelectedIndex = 0;
	static CListBox s_ListBox;
	s_ListBox.SetActive(m_Dialog == DIALOG_NONE && !Ui()->IsPopupOpen());

	const bool GotSelection = s_ListBox.Active() && s_ActionSelectedIndex >= 0 && (size_t)s_ActionSelectedIndex < Map()->m_vSettings.size();

	CUIRect ToolBar, Button, Label, List, DragBar;
	View.HSplitTop(22.0f, &DragBar, nullptr);
	DragBar.y -= 2.0f;
	DragBar.w += 2.0f;
	DragBar.h += 4.0f;
	DoEditorDragBar(View, &DragBar, EDragSide::TOP, &m_aExtraEditorSplits[EXTRAEDITOR_HISTORY]);
	View.HSplitTop(20.0f, &ToolBar, &View);
	View.HSplitTop(2.0f, nullptr, &List);
	ToolBar.HMargin(2.0f, &ToolBar);

	CUIRect TypeButtons, HistoryTypeButton;
	const int HistoryTypeBtnSize = 70.0f;
	ToolBar.VSplitLeft(3 * HistoryTypeBtnSize, &TypeButtons, &Label);

	// history type buttons
	{
		TypeButtons.VSplitLeft(HistoryTypeBtnSize, &HistoryTypeButton, &TypeButtons);
		static int s_EditorHistoryButton = 0;
		if(DoButton_Ex(&s_EditorHistoryButton, "Editor", s_HistoryType == EDITOR_HISTORY, &HistoryTypeButton, BUTTONFLAG_LEFT, "Show map editor history.", IGraphics::CORNER_L))
		{
			s_HistoryType = EDITOR_HISTORY;
		}

		TypeButtons.VSplitLeft(HistoryTypeBtnSize, &HistoryTypeButton, &TypeButtons);
		static int s_EnvelopeEditorHistoryButton = 0;
		if(DoButton_Ex(&s_EnvelopeEditorHistoryButton, "Envelope", s_HistoryType == ENVELOPE_HISTORY, &HistoryTypeButton, BUTTONFLAG_LEFT, "Show envelope editor history.", IGraphics::CORNER_NONE))
		{
			s_HistoryType = ENVELOPE_HISTORY;
		}

		TypeButtons.VSplitLeft(HistoryTypeBtnSize, &HistoryTypeButton, &TypeButtons);
		static int s_ServerSettingsHistoryButton = 0;
		if(DoButton_Ex(&s_ServerSettingsHistoryButton, "Settings", s_HistoryType == SERVER_SETTINGS_HISTORY, &HistoryTypeButton, BUTTONFLAG_LEFT, "Show server settings editor history.", IGraphics::CORNER_R))
		{
			s_HistoryType = SERVER_SETTINGS_HISTORY;
		}
	}

	SLabelProperties InfoProps;
	InfoProps.m_MaxWidth = ToolBar.w - 60.f;
	InfoProps.m_EllipsisAtEnd = true;
	Label.VSplitLeft(8.0f, nullptr, &Label);
	Ui()->DoLabel(&Label, "Editor history. Click on an action to undo all actions above.", 10.0f, TEXTALIGN_ML, InfoProps);

	CEditorHistory *pCurrentHistory;
	if(s_HistoryType == EDITOR_HISTORY)
		pCurrentHistory = &Map()->m_EditorHistory;
	else if(s_HistoryType == ENVELOPE_HISTORY)
		pCurrentHistory = &Map()->m_EnvelopeEditorHistory;
	else if(s_HistoryType == SERVER_SETTINGS_HISTORY)
		pCurrentHistory = &Map()->m_ServerSettingsHistory;
	else
		return;

	// delete button
	ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
	ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
	static int s_DeleteButton = 0;
	if(DoButton_FontIcon(&s_DeleteButton, FontIcon::TRASH, (!pCurrentHistory->m_vpUndoActions.empty() || !pCurrentHistory->m_vpRedoActions.empty()) ? 0 : -1, &Button, BUTTONFLAG_LEFT, "Clear the history.", IGraphics::CORNER_ALL, 9.0f) || (GotSelection && CLineInput::GetActiveInput() == nullptr && m_Dialog == DIALOG_NONE && Ui()->ConsumeHotkey(CUi::HOTKEY_DELETE)))
	{
		pCurrentHistory->Clear();
		s_ActionSelectedIndex = 0;
	}

	// actions list
	int RedoSize = (int)pCurrentHistory->m_vpRedoActions.size();
	int UndoSize = (int)pCurrentHistory->m_vpUndoActions.size();
	s_ActionSelectedIndex = RedoSize;
	s_ListBox.DoStart(15.0f, RedoSize + UndoSize, 1, 3, s_ActionSelectedIndex, &List);

	for(int i = 0; i < RedoSize; i++)
	{
		const CListboxItem Item = s_ListBox.DoNextItem(&pCurrentHistory->m_vpRedoActions[i], s_ActionSelectedIndex >= 0 && s_ActionSelectedIndex == i);
		if(!Item.m_Visible)
			continue;

		Item.m_Rect.VMargin(5.0f, &Label);

		SLabelProperties Props;
		Props.m_MaxWidth = Label.w;
		Props.m_EllipsisAtEnd = true;
		TextRender()->TextColor({.5f, .5f, .5f});
		TextRender()->TextOutlineColor(TextRender()->DefaultTextOutlineColor());
		Ui()->DoLabel(&Label, pCurrentHistory->m_vpRedoActions[i]->DisplayText(), 10.0f, TEXTALIGN_ML, Props);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	for(int i = 0; i < UndoSize; i++)
	{
		const CListboxItem Item = s_ListBox.DoNextItem(&pCurrentHistory->m_vpUndoActions[UndoSize - i - 1], s_ActionSelectedIndex >= RedoSize && s_ActionSelectedIndex == (i + RedoSize));
		if(!Item.m_Visible)
			continue;

		Item.m_Rect.VMargin(5.0f, &Label);

		SLabelProperties Props;
		Props.m_MaxWidth = Label.w;
		Props.m_EllipsisAtEnd = true;
		Ui()->DoLabel(&Label, pCurrentHistory->m_vpUndoActions[UndoSize - i - 1]->DisplayText(), 10.0f, TEXTALIGN_ML, Props);
	}

	{ // Base action "Loaded map" that cannot be undone
		static int s_BaseAction;
		const CListboxItem Item = s_ListBox.DoNextItem(&s_BaseAction, s_ActionSelectedIndex == RedoSize + UndoSize);
		if(Item.m_Visible)
		{
			Item.m_Rect.VMargin(5.0f, &Label);

			Ui()->DoLabel(&Label, "Loaded map", 10.0f, TEXTALIGN_ML);
		}
	}

	const int NewSelected = s_ListBox.DoEnd();
	if(s_ActionSelectedIndex != NewSelected)
	{
		// Figure out if we should undo or redo some actions
		// Undo everything until the selected index
		if(NewSelected > s_ActionSelectedIndex)
		{
			for(int i = 0; i < (NewSelected - s_ActionSelectedIndex); i++)
			{
				pCurrentHistory->Undo();
			}
		}
		else
		{
			for(int i = 0; i < (s_ActionSelectedIndex - NewSelected); i++)
			{
				pCurrentHistory->Redo();
			}
		}
		s_ActionSelectedIndex = NewSelected;
	}
}

void CEditor::DoEditorDragBar(CUIRect View, CUIRect *pDragBar, EDragSide Side, float *pValue, float MinValue, float MaxValue)
{
	enum EDragOperation
	{
		OP_NONE,
		OP_DRAGGING,
		OP_CLICKED
	};
	static EDragOperation s_Operation = OP_NONE;
	static float s_InitialMouseY = 0.0f;
	static float s_InitialMouseOffsetY = 0.0f;
	static float s_InitialMouseX = 0.0f;
	static float s_InitialMouseOffsetX = 0.0f;

	bool IsVertical = Side == EDragSide::TOP || Side == EDragSide::BOTTOM;

	if(Ui()->MouseInside(pDragBar) && Ui()->HotItem() == pDragBar)
		m_CursorType = IsVertical ? CURSOR_RESIZE_V : CURSOR_RESIZE_H;

	bool Clicked;
	bool Abrupted;
	if(int Result = DoButton_DraggableEx(pDragBar, "", 8, pDragBar, &Clicked, &Abrupted, 0, "Change the size of the editor by dragging."))
	{
		if(s_Operation == OP_NONE && Result == 1)
		{
			s_InitialMouseY = Ui()->MouseY();
			s_InitialMouseOffsetY = Ui()->MouseY() - pDragBar->y;
			s_InitialMouseX = Ui()->MouseX();
			s_InitialMouseOffsetX = Ui()->MouseX() - pDragBar->x;
			s_Operation = OP_CLICKED;
		}

		if(Clicked || Abrupted)
			s_Operation = OP_NONE;

		if(s_Operation == OP_CLICKED && absolute(IsVertical ? Ui()->MouseY() - s_InitialMouseY : Ui()->MouseX() - s_InitialMouseX) > 5.0f)
			s_Operation = OP_DRAGGING;

		if(s_Operation == OP_DRAGGING)
		{
			if(Side == EDragSide::TOP)
				*pValue = std::clamp(s_InitialMouseOffsetY + View.y + View.h - Ui()->MouseY(), MinValue, MaxValue);
			else if(Side == EDragSide::RIGHT)
				*pValue = std::clamp(Ui()->MouseX() - s_InitialMouseOffsetX - View.x + pDragBar->w, MinValue, MaxValue);
			else if(Side == EDragSide::BOTTOM)
				*pValue = std::clamp(Ui()->MouseY() - s_InitialMouseOffsetY - View.y + pDragBar->h, MinValue, MaxValue);
			else if(Side == EDragSide::LEFT)
				*pValue = std::clamp(s_InitialMouseOffsetX + View.x + View.w - Ui()->MouseX(), MinValue, MaxValue);

			m_CursorType = IsVertical ? CURSOR_RESIZE_V : CURSOR_RESIZE_H;
		}
	}
}

void CEditor::RenderMenubar(CUIRect MenuBar)
{
	SPopupMenuProperties PopupProperties;
	PopupProperties.m_Corners = IGraphics::CORNER_R | IGraphics::CORNER_B;

	CUIRect FileButton;
	static int s_FileButton = 0;
	MenuBar.VSplitLeft(60.0f, &FileButton, &MenuBar);
	if(DoButton_Ex(&s_FileButton, "File", 0, &FileButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_T, EditorFontSizes::MENU, TEXTALIGN_ML))
	{
		static SPopupMenuId s_PopupMenuFileId;
		Ui()->DoPopupMenu(&s_PopupMenuFileId, FileButton.x, FileButton.y + FileButton.h - 1.0f, 120.0f, 188.0f, this, PopupMenuFile, PopupProperties);
	}

	MenuBar.VSplitLeft(5.0f, nullptr, &MenuBar);

	CUIRect ToolsButton;
	static int s_ToolsButton = 0;
	MenuBar.VSplitLeft(60.0f, &ToolsButton, &MenuBar);
	if(DoButton_Ex(&s_ToolsButton, "Tools", 0, &ToolsButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_T, EditorFontSizes::MENU, TEXTALIGN_ML))
	{
		static SPopupMenuId s_PopupMenuToolsId;
		Ui()->DoPopupMenu(&s_PopupMenuToolsId, ToolsButton.x, ToolsButton.y + ToolsButton.h - 1.0f, 200.0f, 78.0f, this, PopupMenuTools, PopupProperties);
	}

	MenuBar.VSplitLeft(5.0f, nullptr, &MenuBar);

	CUIRect SettingsButton;
	static int s_SettingsButton = 0;
	MenuBar.VSplitLeft(60.0f, &SettingsButton, &MenuBar);
	if(DoButton_Ex(&s_SettingsButton, "Settings", 0, &SettingsButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_T, EditorFontSizes::MENU, TEXTALIGN_ML))
	{
		static SPopupMenuId s_PopupMenuSettingsId;
		Ui()->DoPopupMenu(&s_PopupMenuSettingsId, SettingsButton.x, SettingsButton.y + SettingsButton.h - 1.0f, 280.0f, 148.0f, this, PopupMenuSettings, PopupProperties);
	}

	CUIRect ChangedIndicator, Info, Help, Close;
	MenuBar.VSplitLeft(5.0f, nullptr, &MenuBar);
	MenuBar.VSplitLeft(MenuBar.h, &ChangedIndicator, &MenuBar);
	MenuBar.VSplitRight(15.0f, &MenuBar, &Close);
	MenuBar.VSplitRight(5.0f, &MenuBar, nullptr);
	MenuBar.VSplitRight(15.0f, &MenuBar, &Help);
	MenuBar.VSplitRight(5.0f, &MenuBar, nullptr);
	MenuBar.VSplitLeft(MenuBar.w * 0.6f, &MenuBar, &Info);
	MenuBar.VSplitRight(5.0f, &MenuBar, nullptr);

	if(Map()->m_Modified)
	{
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);
		Ui()->DoLabel(&ChangedIndicator, FontIcon::CIRCLE, 8.0f, TEXTALIGN_MC);
		TextRender()->SetRenderFlags(0);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		static int s_ChangedIndicator;
		DoButtonLogic(&s_ChangedIndicator, 0, &ChangedIndicator, BUTTONFLAG_NONE, "This map has unsaved changes."); // just for the tooltip, result unused
	}

	char aBuf[IO_MAX_PATH_LENGTH + 32];
	str_format(aBuf, sizeof(aBuf), "File: %s", Map()->m_aFilename);
	SLabelProperties Props;
	Props.m_MaxWidth = MenuBar.w;
	Props.m_EllipsisAtEnd = true;
	Ui()->DoLabel(&MenuBar, aBuf, 10.0f, TEXTALIGN_ML, Props);

	char aTimeStr[6];
	str_timestamp_format(aTimeStr, sizeof(aTimeStr), "%H:%M");

	str_format(aBuf, sizeof(aBuf), "X: %.1f, Y: %.1f, Z: %.1f, A: %.1f, G: %i  %s", Ui()->MouseWorldX() / 32.0f, Ui()->MouseWorldY() / 32.0f, MapView()->Zoom()->GetValue(), m_AnimateSpeed, MapView()->MapGrid()->Factor(), aTimeStr);
	Ui()->DoLabel(&Info, aBuf, 10.0f, TEXTALIGN_MR);

	static int s_HelpButton = 0;
	if(DoButton_Editor(&s_HelpButton, "?", 0, &Help, BUTTONFLAG_LEFT, "[F1] Open the DDNet Wiki page for the map editor in a web browser."))
	{
		m_QuickActionShowHelp.Call();
	}

	static int s_CloseButton = 0;
	if(DoButton_Editor(&s_CloseButton, "×", 0, &Close, BUTTONFLAG_LEFT, "[Escape] Exit from the editor."))
	{
		OnClose();
		g_Config.m_ClEditor = 0;
	}
}

void CEditor::ShowHelp()
{
	const char *pLink = Localize("https://wiki.ddnet.org/wiki/Mapping");
	if(!Client()->ViewLink(pLink))
	{
		ShowFileDialogError("Failed to open the link '%s' in the default web browser.", pLink);
	}
}

void CEditor::Render()
{
	// basic start
	Graphics()->Clear(0.0f, 0.0f, 0.0f);
	CUIRect View = *Ui()->Screen();
	Ui()->MapScreen();
	m_CursorType = CURSOR_NORMAL;

	float Width = View.w;
	float Height = View.h;

	// reset tip
	str_copy(m_aTooltip, "");

	// render checker
	RenderBackground(View, m_CheckerTexture, 32.0f, 1.0f);

	CUIRect MenuBar, ModeBar, ToolBar, StatusBar, ExtraEditor, ToolBox;
	m_ShowPicker = Input()->KeyIsPressed(KEY_SPACE) && m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Map()->m_vSelectedLayers.size() == 1;
	if(m_RecentPickerWasOpen && !m_ShowPicker)
		RecentTilesSnapshot();
	m_RecentPickerWasOpen = m_ShowPicker;

	if(m_GuiActive)
	{
		View.HSplitTop(16.0f, &MenuBar, &View);
		View.HSplitTop(53.0f, &ToolBar, &View);
		View.VSplitLeft(m_ToolBoxWidth, &ToolBox, &View);

		View.HSplitBottom(16.0f, &View, &StatusBar);
		if(!m_ShowPicker && m_ActiveExtraEditor != EXTRAEDITOR_NONE)
			View.HSplitBottom(m_aExtraEditorSplits[(int)m_ActiveExtraEditor], &View, &ExtraEditor);
	}
	else
	{
		// hack to get keyboard inputs from toolbar even when GUI is not active
		ToolBar.x = -100;
		ToolBar.y = -100;
		ToolBar.w = 50;
		ToolBar.h = 50;
	}

	//	a little hack for now
	if(m_Mode == MODE_LAYERS)
		DoMapEditor(View);

	if(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
	{
		// handle undo/redo hotkeys
		if(Ui()->CheckActiveItem(nullptr))
		{
			if(Input()->KeyPress(KEY_Z) && Input()->ModifierIsPressed() && !Input()->ShiftIsPressed())
				ActiveHistory().Undo();
			if((Input()->KeyPress(KEY_Y) && Input()->ModifierIsPressed()) || (Input()->KeyPress(KEY_Z) && Input()->ModifierIsPressed() && Input()->ShiftIsPressed()))
				ActiveHistory().Redo();
		}

		// handle brush save/load hotkeys
		for(int i = KEY_1; i <= KEY_0; i++)
		{
			if(Input()->KeyPress(i))
			{
				int Slot = i - KEY_1;
				if(Input()->ModifierIsPressed() && !m_pBrush->IsEmpty())
				{
					dbg_msg("editor", "saving current brush to %d", Slot);
					m_apSavedBrushes[Slot] = std::make_shared<CLayerGroup>(*m_pBrush);
				}
				else if(m_apSavedBrushes[Slot])
				{
					dbg_msg("editor", "loading brush from slot %d", Slot);
					m_pBrush = std::make_shared<CLayerGroup>(*m_apSavedBrushes[Slot]);
				}
			}
		}
	}

	const float BackgroundBrightness = 0.26f;
	const float BackgroundScale = 80.0f;

	if(m_GuiActive)
	{
		RenderBackground(MenuBar, IGraphics::CTextureHandle(), BackgroundScale, 0.0f);
		MenuBar.Margin(2.0f, &MenuBar);

		RenderBackground(ToolBox, g_pData->m_aImages[IMAGE_BACKGROUND_NOISE].m_Id, BackgroundScale, BackgroundBrightness);
		ToolBox.Margin(2.0f, &ToolBox);

		RenderBackground(ToolBar, g_pData->m_aImages[IMAGE_BACKGROUND_NOISE].m_Id, BackgroundScale, BackgroundBrightness);
		ToolBar.Margin(2.0f, &ToolBar);
		ToolBar.VSplitLeft(m_ToolBoxWidth, &ModeBar, &ToolBar);

		RenderBackground(StatusBar, g_pData->m_aImages[IMAGE_BACKGROUND_NOISE].m_Id, BackgroundScale, BackgroundBrightness);
		StatusBar.Margin(2.0f, &StatusBar);
	}

	// do the toolbar
	if(m_Mode == MODE_LAYERS)
		DoToolbarLayers(ToolBar);
	else if(m_Mode == MODE_IMAGES)
		DoToolbarImages(ToolBar);
	else if(m_Mode == MODE_SOUNDS)
		DoToolbarSounds(ToolBar);

	if(m_Dialog == DIALOG_NONE)
	{
		const bool ModPressed = Input()->ModifierIsPressed();
		const bool ShiftPressed = Input()->ShiftIsPressed();
		const bool AltPressed = Input()->AltIsPressed();

		if(CLineInput::GetActiveInput() == nullptr)
		{
			// ctrl+a to append map
			if(Input()->KeyPress(KEY_A) && ModPressed)
			{
				m_FileBrowser.ShowFileDialog(IStorage::TYPE_ALL, CFileBrowser::EFileType::MAP, "Append map", "Append", "maps", "", CallbackAppendMap, this);
			}
		}

		// ctrl+n to create new map
		if(Input()->KeyPress(KEY_N) && ModPressed)
		{
			if(HasUnsavedData())
			{
				if(!m_PopupEventWasActivated)
				{
					m_PopupEventType = POPEVENT_NEW;
					m_PopupEventActivated = true;
				}
			}
			else
			{
				Reset();
			}
		}
		// ctrl+o or ctrl+l to open
		if((Input()->KeyPress(KEY_O) || Input()->KeyPress(KEY_L)) && ModPressed)
		{
			if(ShiftPressed)
			{
				if(!m_QuickActionLoadCurrentMap.Disabled())
				{
					m_QuickActionLoadCurrentMap.Call();
				}
			}
			else
			{
				if(HasUnsavedData())
				{
					if(!m_PopupEventWasActivated)
					{
						m_PopupEventType = POPEVENT_LOAD;
						m_PopupEventActivated = true;
					}
				}
				else
				{
					m_FileBrowser.ShowFileDialog(IStorage::TYPE_ALL, CFileBrowser::EFileType::MAP, "Load map", "Load", "maps", "", CallbackOpenMap, this);
				}
			}
		}

		// ctrl+shift+alt+s to save copy
		if(Input()->KeyPress(KEY_S) && ModPressed && ShiftPressed && AltPressed)
		{
			char aDefaultName[IO_MAX_PATH_LENGTH];
			fs_split_file_extension(fs_filename(Map()->m_aFilename), aDefaultName, sizeof(aDefaultName));
			m_FileBrowser.ShowFileDialog(IStorage::TYPE_SAVE, CFileBrowser::EFileType::MAP, "Save map", "Save copy", "maps", aDefaultName, CallbackSaveCopyMap, this);
		}
		// ctrl+shift+s to save as
		else if(Input()->KeyPress(KEY_S) && ModPressed && ShiftPressed)
		{
			m_QuickActionSaveAs.Call();
		}
		// ctrl+s to save
		else if(Input()->KeyPress(KEY_S) && ModPressed)
		{
			if(Map()->m_aFilename[0] != '\0' && Map()->m_ValidSaveFilename)
			{
				CallbackSaveMap(Map()->m_aFilename, IStorage::TYPE_SAVE, this);
			}
			else
			{
				m_FileBrowser.ShowFileDialog(IStorage::TYPE_SAVE, CFileBrowser::EFileType::MAP, "Save map", "Save", "maps", "", CallbackSaveMap, this);
			}
		}
	}

	if(m_GuiActive)
	{
		CUIRect DragBar;
		ToolBox.VSplitRight(1.0f, &ToolBox, &DragBar);
		DragBar.x -= 2.0f;
		DragBar.w += 4.0f;
		DoEditorDragBar(ToolBox, &DragBar, EDragSide::RIGHT, &m_ToolBoxWidth);

		if(m_Mode == MODE_LAYERS)
			RenderLayers(ToolBox);
		else if(m_Mode == MODE_IMAGES)
		{
			RenderImagesList(ToolBox);
			RenderSelectedImage(View);
		}
		else if(m_Mode == MODE_SOUNDS)
			RenderSounds(ToolBox);
	}

	Ui()->MapScreen();

	CUIRect TooltipRect;
	if(m_GuiActive)
	{
		RenderMenubar(MenuBar);
		RenderModebar(ModeBar);
		if(!m_ShowPicker)
		{
			if(m_ActiveExtraEditor != EXTRAEDITOR_NONE)
			{
				RenderBackground(ExtraEditor, g_pData->m_aImages[IMAGE_BACKGROUND_NOISE].m_Id, BackgroundScale, BackgroundBrightness);
				ExtraEditor.HMargin(2.0f, &ExtraEditor);
				ExtraEditor.VSplitRight(2.0f, &ExtraEditor, nullptr);
			}

			static bool s_ShowServerSettingsEditorLast = false;
			if(m_ActiveExtraEditor == EXTRAEDITOR_ENVELOPES)
			{
				RenderEnvelopeEditor(ExtraEditor);
			}
			else if(m_ActiveExtraEditor == EXTRAEDITOR_SERVER_SETTINGS)
			{
				RenderServerSettingsEditor(ExtraEditor, s_ShowServerSettingsEditorLast);
			}
			else if(m_ActiveExtraEditor == EXTRAEDITOR_HISTORY)
			{
				RenderEditorHistory(ExtraEditor);
			}
			s_ShowServerSettingsEditorLast = m_ActiveExtraEditor == EXTRAEDITOR_SERVER_SETTINGS;
		}
		RenderStatusbar(StatusBar, &TooltipRect);
	}

	RenderPressedKeys(View);
	RenderSavingIndicator(View);

	if(m_Dialog == DIALOG_MAPSETTINGS_ERROR)
	{
		static int s_NullUiTarget = 0;
		Ui()->SetHotItem(&s_NullUiTarget);
		RenderMapSettingsErrorDialog();
	}

	if(m_PopupEventActivated)
	{
		static SPopupMenuId s_PopupEventId;
		constexpr float PopupWidth = 400.0f;
		constexpr float PopupHeight = 150.0f;
		Ui()->DoPopupMenu(&s_PopupEventId, Width / 2.0f - PopupWidth / 2.0f, Height / 2.0f - PopupHeight / 2.0f, PopupWidth, PopupHeight, this, PopupEvent);
		m_PopupEventActivated = false;
		m_PopupEventWasActivated = true;
	}

	if(m_Dialog == DIALOG_NONE && !Ui()->IsPopupHovered() && Ui()->MouseInside(&View))
	{
		// handle zoom hotkeys
		if(CLineInput::GetActiveInput() == nullptr)
		{
			if(Input()->KeyPress(KEY_KP_MINUS))
				MapView()->Zoom()->ChangeValue(50.0f);
			if(Input()->KeyPress(KEY_KP_PLUS))
				MapView()->Zoom()->ChangeValue(-50.0f);
			if(Input()->KeyPress(KEY_KP_MULTIPLY))
				MapView()->ResetZoom();
		}

		if(m_pBrush->IsEmpty() || !Input()->ShiftIsPressed())
		{
			if(Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
				MapView()->Zoom()->ChangeValue(20.0f);
			if(Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
				MapView()->Zoom()->ChangeValue(-20.0f);
		}
		if(!m_pBrush->IsEmpty())
		{
			const bool HasTeleTiles = std::any_of(m_pBrush->m_vpLayers.begin(), m_pBrush->m_vpLayers.end(), [](const auto &pLayer) {
				return pLayer->m_Type == LAYERTYPE_TILES && std::static_pointer_cast<CLayerTiles>(pLayer)->m_HasTele;
			});
			if(HasTeleTiles)
				str_copy(m_aTooltip, "Use shift+mouse wheel up/down to adjust the tele numbers. Use ctrl+f to change all tele numbers to the first unused number.");

			if(Input()->ShiftIsPressed())
			{
				if(Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
					AdjustBrushSpecialTiles(false, -1);
				if(Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
					AdjustBrushSpecialTiles(false, 1);
			}

			// Use ctrl+f to replace number in brush with next free
			if(Input()->ModifierIsPressed() && Input()->KeyPress(KEY_F))
				AdjustBrushSpecialTiles(true);
		}
	}

	for(CEditorComponent &Component : m_vComponents)
		Component.OnRender(View);

	MapView()->UpdateZoom();

	// Cancel color pipette with escape before closing popup menus with escape
	if(m_ColorPipetteActive && Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
	{
		m_ColorPipetteActive = false;
	}

	Ui()->RenderPopupMenus();
	FreeDynamicPopupMenus();

	UpdateColorPipette();

	if(m_Dialog == DIALOG_NONE && !m_PopupEventActivated && Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
	{
		OnClose();
		g_Config.m_ClEditor = 0;
	}

	// The tooltip can be set in popup menus so we have to render the tooltip after the popup menus.
	if(m_GuiActive)
		RenderTooltip(TooltipRect);

	RenderMousePointer();
}

void CEditor::RenderPressedKeys(CUIRect View)
{
	if(!g_Config.m_EdShowkeys)
		return;

	Ui()->MapScreen();
	CTextCursor Cursor;
	Cursor.SetPosition(vec2(View.x + 10, View.y + View.h - 24 - 10));
	Cursor.m_FontSize = 24.0f;

	int NKeys = 0;
	for(int i = 0; i < KEY_LAST; i++)
	{
		if(Input()->KeyIsPressed(i))
		{
			if(NKeys)
				TextRender()->TextEx(&Cursor, " + ", -1);
			TextRender()->TextEx(&Cursor, Input()->KeyName(i), -1);
			NKeys++;
		}
	}
}

void CEditor::RenderSavingIndicator(CUIRect View)
{
	if(m_WriterFinishJobs.empty())
		return;

	const char *pText = "Saving…";
	const float FontSize = 24.0f;

	Ui()->MapScreen();
	CUIRect Label, Spinner;
	View.Margin(20.0f, &View);
	View.HSplitBottom(FontSize, nullptr, &View);
	View.VSplitRight(TextRender()->TextWidth(FontSize, pText) + 2.0f, &Spinner, &Label);
	Spinner.VSplitRight(Spinner.h, nullptr, &Spinner);
	Ui()->DoLabel(&Label, pText, FontSize, TEXTALIGN_MR);
	Ui()->RenderProgressSpinner(Spinner.Center(), 8.0f);
}

void CEditor::FreeDynamicPopupMenus()
{
	auto Iterator = m_PopupMessageContexts.begin();
	while(Iterator != m_PopupMessageContexts.end())
	{
		if(!Ui()->IsPopupOpen(Iterator->second))
		{
			CUi::SMessagePopupContext *pContext = Iterator->second;
			Iterator = m_PopupMessageContexts.erase(Iterator);
			delete pContext;
		}
		else
			++Iterator;
	}
}

void CEditor::UpdateColorPipette()
{
	if(!m_ColorPipetteActive)
		return;

	static char s_PipetteScreenButton;
	if(Ui()->HotItem() == &s_PipetteScreenButton)
	{
		// Read color one pixel to the top and left as we would otherwise not read the correct
		// color due to the cursor sprite being rendered over the current mouse position.
		const int PixelX = std::clamp<int>(round_to_int((Ui()->MouseX() - 1.0f) / Ui()->Screen()->w * Graphics()->ScreenWidth()), 0, Graphics()->ScreenWidth() - 1);
		const int PixelY = std::clamp<int>(round_to_int((Ui()->MouseY() - 1.0f) / Ui()->Screen()->h * Graphics()->ScreenHeight()), 0, Graphics()->ScreenHeight() - 1);
		Graphics()->ReadPixel(ivec2(PixelX, PixelY), &m_PipetteColor);
	}

	// Simulate button overlaying the entire screen to intercept all clicks for color pipette.
	const int ButtonResult = DoButtonLogic(&s_PipetteScreenButton, 0, Ui()->Screen(), BUTTONFLAG_ALL, "Left click to pick a color from the screen. Right click to cancel pipette mode.");
	// Don't handle clicks if we are panning, so the pipette stays active while panning.
	// Checking m_pContainerPanned alone is not enough, as this variable is reset when
	// panning ends before this function is called.
	if(m_pContainerPanned == nullptr && m_pContainerPannedLast == nullptr)
	{
		if(ButtonResult == 1)
		{
			char aClipboard[9];
			str_format(aClipboard, sizeof(aClipboard), "%08X", m_PipetteColor.PackAlphaLast());
			Input()->SetClipboardText(aClipboard);

			// Check if any of the saved colors is equal to the picked color and
			// bring it to the front of the list instead of adding a duplicate.
			int ShiftEnd = (int)std::size(m_aSavedColors) - 1;
			for(int i = 0; i < (int)std::size(m_aSavedColors); ++i)
			{
				if(m_aSavedColors[i].Pack() == m_PipetteColor.Pack())
				{
					ShiftEnd = i;
					break;
				}
			}
			for(int i = ShiftEnd; i > 0; --i)
			{
				m_aSavedColors[i] = m_aSavedColors[i - 1];
			}
			m_aSavedColors[0] = m_PipetteColor;
		}
		if(ButtonResult > 0)
		{
			m_ColorPipetteActive = false;
		}
	}
}

void CEditor::RenderMousePointer()
{
	if(!m_ShowMousePointer)
		return;

	constexpr float CursorSize = 16.0f;

	// Cursor
	Graphics()->WrapClamp();
	Graphics()->TextureSet(m_aCursorTextures[m_CursorType]);
	Graphics()->QuadsBegin();
	if(m_CursorType == CURSOR_RESIZE_V)
	{
		Graphics()->QuadsSetRotation(pi / 2.0f);
	}
	if(m_pUiGotContext == Ui()->HotItem())
	{
		Graphics()->SetColor(1.0f, 0.0f, 0.0f, 1.0f);
	}
	const float CursorOffset = m_CursorType == CURSOR_RESIZE_V || m_CursorType == CURSOR_RESIZE_H ? -CursorSize / 2.0f : 0.0f;
	IGraphics::CQuadItem QuadItem(Ui()->MouseX() + CursorOffset, Ui()->MouseY() + CursorOffset, CursorSize, CursorSize);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();

	// Pipette color
	if(m_ColorPipetteActive)
	{
		CUIRect PipetteRect = {Ui()->MouseX() + CursorSize, Ui()->MouseY() + CursorSize, 80.0f, 20.0f};
		if(PipetteRect.x + PipetteRect.w + 2.0f > Ui()->Screen()->w)
		{
			PipetteRect.x = Ui()->MouseX() - PipetteRect.w - CursorSize / 2.0f;
		}
		if(PipetteRect.y + PipetteRect.h + 2.0f > Ui()->Screen()->h)
		{
			PipetteRect.y = Ui()->MouseY() - PipetteRect.h - CursorSize / 2.0f;
		}
		PipetteRect.Draw(ColorRGBA(0.2f, 0.2f, 0.2f, 0.7f), IGraphics::CORNER_ALL, 3.0f);

		CUIRect Pipette, Label;
		PipetteRect.VSplitLeft(PipetteRect.h, &Pipette, &Label);
		Pipette.Margin(2.0f, &Pipette);
		Pipette.Draw(m_PipetteColor, IGraphics::CORNER_ALL, 3.0f);

		char aLabel[8];
		str_format(aLabel, sizeof(aLabel), "#%06X", m_PipetteColor.PackAlphaLast(false));
		Ui()->DoLabel(&Label, aLabel, 10.0f, TEXTALIGN_MC);
	}
}

void CEditor::RenderGameEntities(const std::shared_ptr<CLayerTiles> &pTiles)
{
	const CGameClient *pGameClient = (CGameClient *)Kernel()->RequestInterface<IGameClient>();
	const float TileSize = 32.f;

	for(int y = 0; y < pTiles->m_Height; y++)
	{
		for(int x = 0; x < pTiles->m_Width; x++)
		{
			const unsigned char Index = pTiles->m_pTiles[y * pTiles->m_Width + x].m_Index - ENTITY_OFFSET;
			if(!((Index >= ENTITY_FLAGSTAND_RED && Index <= ENTITY_WEAPON_LASER) ||
				   (Index >= ENTITY_ARMOR_SHOTGUN && Index <= ENTITY_ARMOR_LASER)))
				continue;

			const bool DDNetOrCustomEntities = std::find_if(std::begin(gs_apModEntitiesNames), std::end(gs_apModEntitiesNames),
								   [&](const char *pEntitiesName) { return str_comp_nocase(m_SelectEntitiesImage.c_str(), pEntitiesName) == 0 &&
													   str_comp_nocase(pEntitiesName, "ddnet") != 0; }) == std::end(gs_apModEntitiesNames);

			vec2 Pos(x * TileSize, y * TileSize);
			vec2 Scale;
			int VisualSize;

			if(Index == ENTITY_FLAGSTAND_RED)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpriteFlagRed);
				Scale = vec2(42, 84);
				VisualSize = 1;
				Pos.y -= (Scale.y / 2.f) * 0.75f;
			}
			else if(Index == ENTITY_FLAGSTAND_BLUE)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpriteFlagBlue);
				Scale = vec2(42, 84);
				VisualSize = 1;
				Pos.y -= (Scale.y / 2.f) * 0.75f;
			}
			else if(Index == ENTITY_ARMOR_1)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpritePickupArmor);
				Graphics()->GetSpriteScale(SPRITE_PICKUP_HEALTH, Scale.x, Scale.y);
				VisualSize = 64;
			}
			else if(Index == ENTITY_HEALTH_1)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpritePickupHealth);
				Graphics()->GetSpriteScale(SPRITE_PICKUP_HEALTH, Scale.x, Scale.y);
				VisualSize = 64;
			}
			else if(Index == ENTITY_WEAPON_SHOTGUN)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_aSpritePickupWeapons[WEAPON_SHOTGUN]);
				Graphics()->GetSpriteScale(SPRITE_PICKUP_SHOTGUN, Scale.x, Scale.y);
				VisualSize = g_pData->m_Weapons.m_aId[WEAPON_SHOTGUN].m_VisualSize;
			}
			else if(Index == ENTITY_WEAPON_GRENADE)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_aSpritePickupWeapons[WEAPON_GRENADE]);
				Graphics()->GetSpriteScale(SPRITE_PICKUP_GRENADE, Scale.x, Scale.y);
				VisualSize = g_pData->m_Weapons.m_aId[WEAPON_GRENADE].m_VisualSize;
			}
			else if(Index == ENTITY_WEAPON_LASER)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_aSpritePickupWeapons[WEAPON_LASER]);
				Graphics()->GetSpriteScale(SPRITE_PICKUP_LASER, Scale.x, Scale.y);
				VisualSize = g_pData->m_Weapons.m_aId[WEAPON_LASER].m_VisualSize;
			}
			else if(Index == ENTITY_POWERUP_NINJA)
			{
				Graphics()->TextureSet(pGameClient->m_GameSkin.m_aSpritePickupWeapons[WEAPON_NINJA]);
				Graphics()->GetSpriteScale(SPRITE_PICKUP_NINJA, Scale.x, Scale.y);
				VisualSize = 128;
			}
			else if(DDNetOrCustomEntities)
			{
				if(Index == ENTITY_ARMOR_SHOTGUN)
				{
					Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpritePickupArmorShotgun);
					Graphics()->GetSpriteScale(SPRITE_PICKUP_ARMOR_SHOTGUN, Scale.x, Scale.y);
					VisualSize = 64;
				}
				else if(Index == ENTITY_ARMOR_GRENADE)
				{
					Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpritePickupArmorGrenade);
					Graphics()->GetSpriteScale(SPRITE_PICKUP_ARMOR_GRENADE, Scale.x, Scale.y);
					VisualSize = 64;
				}
				else if(Index == ENTITY_ARMOR_NINJA)
				{
					Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpritePickupArmorNinja);
					Graphics()->GetSpriteScale(SPRITE_PICKUP_ARMOR_NINJA, Scale.x, Scale.y);
					VisualSize = 64;
				}
				else if(Index == ENTITY_ARMOR_LASER)
				{
					Graphics()->TextureSet(pGameClient->m_GameSkin.m_SpritePickupArmorLaser);
					Graphics()->GetSpriteScale(SPRITE_PICKUP_ARMOR_LASER, Scale.x, Scale.y);
					VisualSize = 64;
				}
				else
					continue;
			}
			else
				continue;

			Graphics()->QuadsBegin();

			if(Index != ENTITY_FLAGSTAND_RED && Index != ENTITY_FLAGSTAND_BLUE)
			{
				const unsigned char Flags = pTiles->m_pTiles[y * pTiles->m_Width + x].m_Flags;

				if(Flags & TILEFLAG_XFLIP)
					Scale.x = -Scale.x;

				if(Flags & TILEFLAG_YFLIP)
					Scale.y = -Scale.y;

				if(Flags & TILEFLAG_ROTATE)
				{
					Graphics()->QuadsSetRotation(90.f * (pi / 180));

					if(Index == ENTITY_POWERUP_NINJA)
					{
						if(Flags & TILEFLAG_XFLIP)
							Pos.y += 10.0f;
						else
							Pos.y -= 10.0f;
					}
				}
				else
				{
					if(Index == ENTITY_POWERUP_NINJA)
					{
						if(Flags & TILEFLAG_XFLIP)
							Pos.x += 10.0f;
						else
							Pos.x -= 10.0f;
					}
				}
			}

			Scale *= VisualSize;
			Pos -= vec2((Scale.x - TileSize) / 2.f, (Scale.y - TileSize) / 2.f);
			Pos += direction(Client()->GlobalTime() * 2.0f + x + y) * 2.5f;

			IGraphics::CQuadItem Quad(Pos.x, Pos.y, Scale.x, Scale.y);
			Graphics()->QuadsDrawTL(&Quad, 1);
			Graphics()->QuadsEnd();
		}
	}
}

void CEditor::RenderSwitchEntities(const std::shared_ptr<CLayerTiles> &pTiles)
{
	const CGameClient *pGameClient = (CGameClient *)Kernel()->RequestInterface<IGameClient>();
	const float TileSize = 32.f;

	std::function<unsigned char(int, int, unsigned char &)> GetIndex;
	if(pTiles->m_HasSwitch)
	{
		CLayerSwitch *pSwitchLayer = ((CLayerSwitch *)(pTiles.get()));
		GetIndex = [pSwitchLayer](int y, int x, unsigned char &Number) -> unsigned char {
			if(x < 0 || y < 0 || x >= pSwitchLayer->m_Width || y >= pSwitchLayer->m_Height)
				return 0;
			Number = pSwitchLayer->m_pSwitchTile[y * pSwitchLayer->m_Width + x].m_Number;
			return pSwitchLayer->m_pSwitchTile[y * pSwitchLayer->m_Width + x].m_Type - ENTITY_OFFSET;
		};
	}
	else
	{
		GetIndex = [pTiles](int y, int x, unsigned char &Number) -> unsigned char {
			if(x < 0 || y < 0 || x >= pTiles->m_Width || y >= pTiles->m_Height)
				return 0;
			Number = 0;
			return pTiles->m_pTiles[y * pTiles->m_Width + x].m_Index - ENTITY_OFFSET;
		};
	}

	ivec2 aOffsets[] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

	const ColorRGBA OuterColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClLaserDoorOutlineColor));
	const ColorRGBA InnerColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClLaserDoorInnerColor));
	const float TicksHead = Client()->GlobalTime() * Client()->GameTickSpeed();

	for(int y = 0; y < pTiles->m_Height; y++)
	{
		for(int x = 0; x < pTiles->m_Width; x++)
		{
			unsigned char Number = 0;
			const unsigned char Index = GetIndex(y, x, Number);

			if(Index == ENTITY_DOOR)
			{
				for(size_t i = 0; i < sizeof(aOffsets) / sizeof(ivec2); ++i)
				{
					unsigned char NumberDoorLength = 0;
					unsigned char IndexDoorLength = GetIndex(y + aOffsets[i].y, x + aOffsets[i].x, NumberDoorLength);
					if(IndexDoorLength >= ENTITY_LASER_SHORT && IndexDoorLength <= ENTITY_LASER_LONG)
					{
						float XOff = std::cos(i * pi / 4.0f);
						float YOff = std::sin(i * pi / 4.0f);
						int Length = (IndexDoorLength - ENTITY_LASER_SHORT + 1) * 3;
						vec2 Pos(x + 0.5f, y + 0.5f);
						vec2 To(x + XOff * Length + 0.5f, y + YOff * Length + 0.5f);
						pGameClient->m_Items.RenderLaser(To * TileSize, Pos * TileSize, OuterColor, InnerColor, 1.0f, TicksHead, (int)LASERTYPE_DOOR);
					}
				}
			}
		}
	}
}

void CEditor::Reset(bool CreateDefault)
{
	Ui()->ClosePopupMenus();
	Map()->Clean();

	for(CEditorComponent &Component : m_vComponents)
		Component.OnReset();

	m_ToolbarPreviewSound = -1;

	// create default layers
	if(CreateDefault)
	{
		m_EditorWasUsedBefore = true;
		Map()->CreateDefault();
	}

	m_pContainerPanned = nullptr;
	m_pContainerPannedLast = nullptr;

	m_ActiveEnvelopePreview = EEnvelopePreview::NONE;
	m_QuadEnvelopePointOperation = EQuadEnvelopePointOperation::NONE;

	m_ResetZoomEnvelope = true;
	m_SettingsCommandInput.Clear();
	m_MapSettingsCommandContext.Reset();
}

int CEditor::GetTextureUsageFlag() const
{
	return Graphics()->Uses2DTextureArrays() ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE : IGraphics::TEXLOAD_TO_3D_TEXTURE;
}

IGraphics::CTextureHandle CEditor::GetFrontTexture()
{
	if(!m_FrontTexture.IsValid())
		m_FrontTexture = Graphics()->LoadTexture("editor/front.png", IStorage::TYPE_ALL, GetTextureUsageFlag());
	return m_FrontTexture;
}

IGraphics::CTextureHandle CEditor::GetTeleTexture()
{
	if(!m_TeleTexture.IsValid())
		m_TeleTexture = Graphics()->LoadTexture("editor/tele.png", IStorage::TYPE_ALL, GetTextureUsageFlag());
	return m_TeleTexture;
}

IGraphics::CTextureHandle CEditor::GetSpeedupTexture()
{
	if(!m_SpeedupTexture.IsValid())
		m_SpeedupTexture = Graphics()->LoadTexture("editor/speedup.png", IStorage::TYPE_ALL, GetTextureUsageFlag());
	return m_SpeedupTexture;
}

IGraphics::CTextureHandle CEditor::GetSwitchTexture()
{
	if(!m_SwitchTexture.IsValid())
		m_SwitchTexture = Graphics()->LoadTexture("editor/switch.png", IStorage::TYPE_ALL, GetTextureUsageFlag());
	return m_SwitchTexture;
}

IGraphics::CTextureHandle CEditor::GetTuneTexture()
{
	if(!m_TuneTexture.IsValid())
		m_TuneTexture = Graphics()->LoadTexture("editor/tune.png", IStorage::TYPE_ALL, GetTextureUsageFlag());
	return m_TuneTexture;
}

IGraphics::CTextureHandle CEditor::GetEntitiesTexture()
{
	if(!m_EntitiesTexture.IsValid())
		m_EntitiesTexture = Graphics()->LoadTexture("editor/entities/DDNet.png", IStorage::TYPE_ALL, GetTextureUsageFlag());
	return m_EntitiesTexture;
}

void CEditor::Init()
{
	m_pInput = Kernel()->RequestInterface<IInput>();
	m_pClient = Kernel()->RequestInterface<IClient>();
	m_pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_pConfig = m_pConfigManager->Values();
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pGraphics = Kernel()->RequestInterface<IGraphics>();
	m_pTextRender = Kernel()->RequestInterface<ITextRender>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pSound = Kernel()->RequestInterface<ISound>();
	m_UI.Init(Kernel());
	m_UI.SetPopupMenuClosedCallback([this]() {
		m_PopupEventWasActivated = false;
	});
	m_RenderMap.Init(m_pGraphics, m_pTextRender);
	m_ZoomEnvelopeX.OnInit(this);
	m_ZoomEnvelopeY.OnInit(this);

	m_vComponents.emplace_back(m_MapView);
	m_vComponents.emplace_back(m_MapSettingsBackend);
	m_vComponents.emplace_back(m_LayerSelector);
	m_vComponents.emplace_back(m_FileBrowser);
	m_vComponents.emplace_back(m_Prompt);
	m_vComponents.emplace_back(m_FontTyper);
	for(CEditorComponent &Component : m_vComponents)
		Component.OnInit(this);

	m_CheckerTexture = Graphics()->LoadTexture("editor/checker.png", IStorage::TYPE_ALL);
	m_aCursorTextures[CURSOR_NORMAL] = Graphics()->LoadTexture("editor/cursor.png", IStorage::TYPE_ALL);
	m_aCursorTextures[CURSOR_RESIZE_H] = Graphics()->LoadTexture("editor/cursor_resize.png", IStorage::TYPE_ALL);
	m_aCursorTextures[CURSOR_RESIZE_V] = m_aCursorTextures[CURSOR_RESIZE_H];

	m_pTilesetPicker = std::make_shared<CLayerTiles>(Map(), 16, 16);
	m_pTilesetPicker->MakePalette();
	m_pTilesetPicker->m_Readonly = true;

	m_pQuadsetPicker = std::make_shared<CLayerQuads>(Map());
	m_pQuadsetPicker->NewQuad(0, 0, 64, 64);
	m_pQuadsetPicker->m_Readonly = true;

	m_pBrush = std::make_shared<CLayerGroup>(Map());

	Reset(false);
}

void CEditor::HandleCursorMovement()
{
	const vec2 UpdatedMousePos = Ui()->UpdatedMousePos();
	const vec2 UpdatedMouseDelta = Ui()->UpdatedMouseDelta();

	// fix correct world x and y
	const std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
	if(pGroup)
	{
		float aPoints[4];
		pGroup->Mapping(aPoints);

		float WorldWidth = aPoints[2] - aPoints[0];
		float WorldHeight = aPoints[3] - aPoints[1];

		m_MouseWorldScale = WorldWidth / Graphics()->WindowWidth();

		m_MouseWorldPos.x = aPoints[0] + WorldWidth * (UpdatedMousePos.x / Graphics()->WindowWidth());
		m_MouseWorldPos.y = aPoints[1] + WorldHeight * (UpdatedMousePos.y / Graphics()->WindowHeight());
		m_MouseDeltaWorld.x = UpdatedMouseDelta.x * (WorldWidth / Graphics()->WindowWidth());
		m_MouseDeltaWorld.y = UpdatedMouseDelta.y * (WorldHeight / Graphics()->WindowHeight());
	}
	else
	{
		m_MouseWorldPos = vec2(-1.0f, -1.0f);
		m_MouseDeltaWorld = vec2(0.0f, 0.0f);
	}

	m_MouseWorldNoParaPos = vec2(-1.0f, -1.0f);
	for(const std::shared_ptr<CLayerGroup> &pGameGroup : Map()->m_vpGroups)
	{
		if(!pGameGroup->m_GameGroup)
			continue;

		float aPoints[4];
		pGameGroup->Mapping(aPoints);

		float WorldWidth = aPoints[2] - aPoints[0];
		float WorldHeight = aPoints[3] - aPoints[1];

		m_MouseWorldNoParaPos.x = aPoints[0] + WorldWidth * (UpdatedMousePos.x / Graphics()->WindowWidth());
		m_MouseWorldNoParaPos.y = aPoints[1] + WorldHeight * (UpdatedMousePos.y / Graphics()->WindowHeight());
	}

	OnMouseMove(UpdatedMousePos);
}

void CEditor::OnMouseMove(vec2 MousePos)
{
	m_vHoverTiles.clear();
	for(size_t g = 0; g < Map()->m_vpGroups.size(); g++)
	{
		const std::shared_ptr<CLayerGroup> pGroup = Map()->m_vpGroups[g];
		for(size_t l = 0; l < pGroup->m_vpLayers.size(); l++)
		{
			const std::shared_ptr<CLayer> pLayer = pGroup->m_vpLayers[l];
			int LayerType = pLayer->m_Type;
			if(LayerType != LAYERTYPE_TILES &&
				LayerType != LAYERTYPE_FRONT &&
				LayerType != LAYERTYPE_TELE &&
				LayerType != LAYERTYPE_SPEEDUP &&
				LayerType != LAYERTYPE_SWITCH &&
				LayerType != LAYERTYPE_TUNE)
				continue;

			std::shared_ptr<CLayerTiles> pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
			pGroup->MapScreen();
			float aPoints[4];
			pGroup->Mapping(aPoints);
			float WorldWidth = aPoints[2] - aPoints[0];
			float WorldHeight = aPoints[3] - aPoints[1];
			CUIRect Rect;
			Rect.x = aPoints[0] + WorldWidth * (MousePos.x / Graphics()->WindowWidth());
			Rect.y = aPoints[1] + WorldHeight * (MousePos.y / Graphics()->WindowHeight());
			Rect.w = 0;
			Rect.h = 0;
			CIntRect r;
			pTiles->Convert(Rect, &r);
			pTiles->Clamp(&r);
			int x = r.x;
			int y = r.y;

			if(x < 0 || x >= pTiles->m_Width)
				continue;
			if(y < 0 || y >= pTiles->m_Height)
				continue;
			CTile Tile = pTiles->GetTile(x, y);
			if(Tile.m_Index)
				m_vHoverTiles.emplace_back(
					g, l, x, y, Tile);
		}
	}
	Ui()->MapScreen();
}

void CEditor::MouseAxisLock(vec2 &CursorRel)
{
	if(Input()->AltIsPressed())
	{
		// only lock with the paint brush and inside editor map area to avoid duplicate Alt behavior
		if(m_pBrush->IsEmpty() || Ui()->HotItem() != &m_MapEditorId)
			return;

		const vec2 CurrentWorldPos = vec2(Ui()->MouseWorldX(), Ui()->MouseWorldY()) / 32.0f;

		if(m_MouseAxisLockState == EAxisLock::START)
		{
			m_MouseAxisInitialPos = CurrentWorldPos;
			m_MouseAxisLockState = EAxisLock::NONE;
			return; // delta would be 0, calculate it in next frame
		}

		const vec2 Delta = CurrentWorldPos - m_MouseAxisInitialPos;

		// lock to axis if moved mouse by 1 block
		if(m_MouseAxisLockState == EAxisLock::NONE && (std::abs(Delta.x) > 1.0f || std::abs(Delta.y) > 1.0f))
		{
			m_MouseAxisLockState = (std::abs(Delta.x) > std::abs(Delta.y)) ? EAxisLock::HORIZONTAL : EAxisLock::VERTICAL;
		}

		if(m_MouseAxisLockState == EAxisLock::HORIZONTAL)
		{
			CursorRel.y = 0;
		}
		else if(m_MouseAxisLockState == EAxisLock::VERTICAL)
		{
			CursorRel.x = 0;
		}
	}
	else
	{
		m_MouseAxisLockState = EAxisLock::START;
	}
}

void CEditor::HandleAutosave()
{
	const float Time = Client()->GlobalTime();
	const float LastAutosaveUpdateTime = m_LastAutosaveUpdateTime;
	m_LastAutosaveUpdateTime = Time;

	if(g_Config.m_EdAutosaveInterval == 0)
		return; // autosave disabled
	if(!Map()->m_ModifiedAuto || Map()->m_LastModifiedTime < 0.0f)
		return; // no unsaved changes

	// Add time to autosave timer if the editor was disabled for more than 10 seconds,
	// to prevent autosave from immediately activating when the editor is activated
	// after being deactivated for some time.
	if(LastAutosaveUpdateTime >= 0.0f && Time - LastAutosaveUpdateTime > 10.0f)
	{
		Map()->m_LastSaveTime += Time - LastAutosaveUpdateTime;
	}

	// Check if autosave timer has expired.
	if(Map()->m_LastSaveTime >= Time || Time - Map()->m_LastSaveTime < 60 * g_Config.m_EdAutosaveInterval)
		return;

	// Wait for 5 seconds of no modification before saving, to prevent autosave
	// from immediately activating when a map is first modified or while user is
	// modifying the map, but don't delay the autosave for more than 1 minute.
	if(Time - Map()->m_LastModifiedTime < 5.0f && Time - Map()->m_LastSaveTime < 60 * (g_Config.m_EdAutosaveInterval + 1))
		return;

	const auto &&ErrorHandler = [this](const char *pErrorMessage) {
		ShowFileDialogError("%s", pErrorMessage);
		log_error("editor/autosave", "%s", pErrorMessage);
	};
	Map()->PerformAutosave(ErrorHandler);
}

void CEditor::HandleWriterFinishJobs()
{
	if(m_WriterFinishJobs.empty())
		return;

	std::shared_ptr<CDataFileWriterFinishJob> pJob = m_WriterFinishJobs.front();
	if(!pJob->Done())
		return;
	m_WriterFinishJobs.pop_front();

	const char *pErrorMessage = pJob->ErrorMessage();
	if(pErrorMessage[0] != '\0')
	{
		ShowFileDialogError("%s", pErrorMessage);
		return;
	}

	// send rcon.. if we can
	if(Client()->RconAuthed() && g_Config.m_EdAutoMapReload)
	{
		CServerInfo CurrentServerInfo;
		Client()->GetServerInfo(&CurrentServerInfo);

		if(net_addr_is_local(&Client()->ServerAddress()))
		{
			char aMapName[MAX_MAP_LENGTH];
			IStorage::StripPathAndExtension(pJob->RealFilename(), aMapName, sizeof(aMapName));
			if(!str_comp(aMapName, CurrentServerInfo.m_aMap))
				Client()->Rcon("hot_reload");
		}
	}
}

void CEditor::OnUpdate()
{
	CUIElementBase::Init(Ui()); // update static pointer because game and editor use separate UI

	if(!m_EditorWasUsedBefore)
	{
		m_EditorWasUsedBefore = true;
		Reset();
	}

	m_pContainerPannedLast = m_pContainerPanned;

	// handle mouse movement
	vec2 CursorRel = vec2(0.0f, 0.0f);
	IInput::ECursorType CursorType = Input()->CursorRelative(&CursorRel.x, &CursorRel.y);
	if(CursorType != IInput::CURSOR_NONE)
	{
		Ui()->ConvertMouseMove(&CursorRel.x, &CursorRel.y, CursorType);
		MouseAxisLock(CursorRel);
		Ui()->OnCursorMove(CursorRel.x, CursorRel.y);
	}

	// handle key presses
	Input()->ConsumeEvents([&](const IInput::CEvent &Event) {
		if(m_Dialog == DIALOG_NONE &&
			CLineInput::GetActiveInput() == nullptr &&
			Event.m_Key == KEY_F1)
		{
			if((Event.m_Flags & IInput::FLAG_PRESS) != 0 &&
				(Event.m_Flags & IInput::FLAG_REPEAT) == 0)
			{
				m_QuickActionShowHelp.Call();
			}
			return;
		}

		for(CEditorComponent &Component : m_vComponents)
		{
			// Events with flag `FLAG_RELEASE` must always be forwarded to all components so keys being
			// released can be handled in all components also after some components have been disabled.
			if(Component.OnInput(Event) && (Event.m_Flags & ~IInput::FLAG_RELEASE) != 0)
				return;
		}
		Ui()->OnInput(Event);
	});

	HandleCursorMovement();
	HandleAutosave();
	HandleWriterFinishJobs();

	for(CEditorComponent &Component : m_vComponents)
		Component.OnUpdate();
}

void CEditor::OnRender()
{
	Ui()->SetMouseSlow(false);

	// toggle gui
	if(m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_TAB))
		m_GuiActive = !m_GuiActive;

	if(Input()->KeyPress(KEY_F10))
		m_ShowMousePointer = false;

	if(m_Animate)
		m_AnimateTime = (time_get() - m_AnimateStart) / (float)time_freq();
	else
		m_AnimateTime = 0;

	m_pUiGotContext = nullptr;
	Ui()->StartCheck();

	Ui()->Update(m_MouseWorldPos);

	Render();

	m_MouseDeltaWorld = vec2(0.0f, 0.0f);

	if(Input()->KeyPress(KEY_F10))
	{
		Graphics()->TakeScreenshot(nullptr);
		m_ShowMousePointer = true;
	}

	if(g_Config.m_Debug)
		Ui()->DebugRender(2.0f, Ui()->Screen()->h - 27.0f);

	Ui()->FinishCheck();
	Ui()->ClearHotkeys();
	Input()->Clear();

	CLineInput::RenderCandidates();

#if defined(CONF_DEBUG)
	Map()->CheckIntegrity();
#endif
}

void CEditor::OnActivate()
{
	ResetMentions();
	ResetIngameMoved();
}

void CEditor::OnWindowResize()
{
	Ui()->OnWindowResize();
}

void CEditor::OnClose()
{
	m_ColorPipetteActive = false;

	if(m_ToolbarPreviewSound >= 0 && Sound()->IsPlaying(m_ToolbarPreviewSound))
		Sound()->Pause(m_ToolbarPreviewSound);

	m_FileBrowser.OnEditorClose();
}

void CEditor::OnDialogClose()
{
	m_Dialog = DIALOG_NONE;
	m_FileBrowser.OnDialogClose();
}

void CEditor::LoadCurrentMap()
{
	CGameClient *pGameClient = (CGameClient *)Kernel()->RequestInterface<IGameClient>();

	if(Load(pGameClient->Map()->Path(), IStorage::TYPE_SAVE))
	{
		Map()->m_ValidSaveFilename = !str_startswith(pGameClient->Map()->Path(), "downloadedmaps/");
	}
	else
	{
		Load(pGameClient->Map()->Path(), IStorage::TYPE_ALL);
		Map()->m_ValidSaveFilename = false;
	}

	vec2 Center = pGameClient->m_Camera.m_Center;
	MapView()->SetWorldOffset(Center);
}

bool CEditor::Save(const char *pFilename)
{
	// Check if file with this name is already being saved at the moment
	if(std::any_of(std::begin(m_WriterFinishJobs), std::end(m_WriterFinishJobs), [pFilename](const std::shared_ptr<CDataFileWriterFinishJob> &Job) {
		   return str_comp(pFilename, Job->RealFilename()) == 0;
	   }))
	{
		return false;
	}

	const auto &&ErrorHandler = [this](const char *pErrorMessage) {
		ShowFileDialogError("%s", pErrorMessage);
		log_error("editor/save", "%s", pErrorMessage);
	};
	return Map()->Save(pFilename, ErrorHandler);
}

bool CEditor::HandleMapDrop(const char *pFilename, int StorageType)
{
	OnDialogClose();
	if(HasUnsavedData())
	{
		str_copy(m_aFilenamePendingLoad, pFilename);
		m_PopupEventType = CEditor::POPEVENT_LOADDROP;
		m_PopupEventActivated = true;
		return true;
	}
	else
	{
		return Load(pFilename, IStorage::TYPE_ALL_OR_ABSOLUTE);
	}
}

bool CEditor::Load(const char *pFilename, int StorageType)
{
	const auto &&ErrorHandler = [this](const char *pErrorMessage) {
		ShowFileDialogError("%s", pErrorMessage);
		log_error("editor/load", "%s", pErrorMessage);
	};

	Reset();
	bool Result = Map()->Load(pFilename, StorageType, std::move(ErrorHandler));
	if(Result)
	{
		for(CEditorComponent &Component : m_vComponents)
			Component.OnMapLoad();

		log_info("editor/load", "Loaded map '%s'", Map()->m_aFilename);
	}
	return Result;
}

CEditorHistory &CEditor::ActiveHistory()
{
	if(m_ActiveExtraEditor == EXTRAEDITOR_SERVER_SETTINGS)
	{
		return Map()->m_ServerSettingsHistory;
	}
	else if(m_ActiveExtraEditor == EXTRAEDITOR_ENVELOPES)
	{
		return Map()->m_EnvelopeEditorHistory;
	}
	else
	{
		return Map()->m_EditorHistory;
	}
}

void CEditor::AdjustBrushSpecialTiles(bool UseNextFree, int Adjust)
{
	// Adjust m_Angle of speedup or m_Number field of tune, switch and tele tiles by `Adjust` if `UseNextFree` is false
	// If `Adjust` is 0 and `UseNextFree` is false, then update numbers of brush tiles to global values
	// If true, then use the next free number instead

	auto &&AdjustNumber = [Adjust](auto &Number, short Limit = 255) {
		Number = ((Number + Adjust) - 1 + Limit) % Limit + 1;
	};

	for(auto &pLayer : m_pBrush->m_vpLayers)
	{
		if(pLayer->m_Type != LAYERTYPE_TILES)
			continue;

		std::shared_ptr<CLayerTiles> pLayerTiles = std::static_pointer_cast<CLayerTiles>(pLayer);

		if(pLayerTiles->m_HasTele)
		{
			int NextFreeTeleNumber = Map()->m_pTeleLayer->FindNextFreeNumber(false);
			int NextFreeCPNumber = Map()->m_pTeleLayer->FindNextFreeNumber(true);
			std::shared_ptr<CLayerTele> pTeleLayer = std::static_pointer_cast<CLayerTele>(pLayer);

			for(int y = 0; y < pTeleLayer->m_Height; y++)
			{
				for(int x = 0; x < pTeleLayer->m_Width; x++)
				{
					int i = y * pTeleLayer->m_Width + x;
					if(!IsValidTeleTile(pTeleLayer->m_pTiles[i].m_Index) || (!UseNextFree && !pTeleLayer->m_pTeleTile[i].m_Number))
						continue;

					if(UseNextFree)
					{
						if(IsTeleTileCheckpoint(pTeleLayer->m_pTiles[i].m_Index))
							pTeleLayer->m_pTeleTile[i].m_Number = NextFreeCPNumber;
						else if(IsTeleTileNumberUsedAny(pTeleLayer->m_pTiles[i].m_Index))
							pTeleLayer->m_pTeleTile[i].m_Number = NextFreeTeleNumber;
					}
					else
						AdjustNumber(pTeleLayer->m_pTeleTile[i].m_Number);

					if(!UseNextFree && Adjust == 0 && IsTeleTileNumberUsedAny(pTeleLayer->m_pTiles[i].m_Index))
					{
						if(IsTeleTileCheckpoint(pTeleLayer->m_pTiles[i].m_Index))
							pTeleLayer->m_pTeleTile[i].m_Number = m_TeleCheckpointNumber;
						else
							pTeleLayer->m_pTeleTile[i].m_Number = m_TeleNumber;
					}
				}
			}
		}
		else if(pLayerTiles->m_HasTune)
		{
			if(!UseNextFree)
			{
				std::shared_ptr<CLayerTune> pTuneLayer = std::static_pointer_cast<CLayerTune>(pLayer);
				for(int y = 0; y < pTuneLayer->m_Height; y++)
				{
					for(int x = 0; x < pTuneLayer->m_Width; x++)
					{
						int i = y * pTuneLayer->m_Width + x;
						if(!IsValidTuneTile(pTuneLayer->m_pTiles[i].m_Index) || !pTuneLayer->m_pTuneTile[i].m_Number)
							continue;

						AdjustNumber(pTuneLayer->m_pTuneTile[i].m_Number);
					}
				}
			}
		}
		else if(pLayerTiles->m_HasSwitch)
		{
			int NextFreeNumber = Map()->m_pSwitchLayer->FindNextFreeNumber();
			std::shared_ptr<CLayerSwitch> pSwitchLayer = std::static_pointer_cast<CLayerSwitch>(pLayer);

			for(int y = 0; y < pSwitchLayer->m_Height; y++)
			{
				for(int x = 0; x < pSwitchLayer->m_Width; x++)
				{
					int i = y * pSwitchLayer->m_Width + x;
					if(!IsValidSwitchTile(pSwitchLayer->m_pTiles[i].m_Index) || (!UseNextFree && !pSwitchLayer->m_pSwitchTile[i].m_Number))
						continue;

					if(UseNextFree)
						pSwitchLayer->m_pSwitchTile[i].m_Number = NextFreeNumber;
					else
						AdjustNumber(pSwitchLayer->m_pSwitchTile[i].m_Number);
				}
			}
		}
		else if(pLayerTiles->m_HasSpeedup)
		{
			if(!UseNextFree)
			{
				std::shared_ptr<CLayerSpeedup> pSpeedupLayer = std::static_pointer_cast<CLayerSpeedup>(pLayer);
				for(int y = 0; y < pSpeedupLayer->m_Height; y++)
				{
					for(int x = 0; x < pSpeedupLayer->m_Width; x++)
					{
						int i = y * pSpeedupLayer->m_Width + x;
						if(!IsValidSpeedupTile(pSpeedupLayer->m_pTiles[i].m_Index))
							continue;

						if(Adjust != 0)
						{
							AdjustNumber(pSpeedupLayer->m_pSpeedupTile[i].m_Angle, 359);
						}
						else
						{
							pSpeedupLayer->m_pSpeedupTile[i].m_Angle = m_SpeedupAngle;
							pSpeedupLayer->m_SpeedupAngle = m_SpeedupAngle;
						}
					}
				}
			}
		}
	}
}

vec2 CEditor::CatmullRom(vec2 P0, vec2 P1, vec2 P2, vec2 P3, float t)
{
	const float t2 = t * t;
	const float t3 = t2 * t;
	const vec2 A = P1 * 2.0f;
	const vec2 B = (P2 - P0) * t;
	const vec2 C = (P0 * 2.0f - P1 * 5.0f + P2 * 4.0f - P3) * t2;
	const vec2 D = (P1 * 3.0f - P0 - P2 * 3.0f + P3) * t3;
	return (A + B + C + D) * 0.5f;
}

void CEditor::LineRecomputeAutoHandles(int NodeIndex)
{
	const int N = (int)m_vLineNodes.size();
	if(NodeIndex < 0 || NodeIndex >= N)
		return;
	if((int)m_vLineHandleIn.size() != N)
		m_vLineHandleIn.resize(N, vec2(0.0f, 0.0f));
	if((int)m_vLineHandleOut.size() != N)
		m_vLineHandleOut.resize(N, vec2(0.0f, 0.0f));
	const vec2 P = m_vLineNodes[NodeIndex];
	const vec2 Prev = (NodeIndex > 0) ? m_vLineNodes[NodeIndex - 1] : (P + (P - m_vLineNodes[std::min(N - 1, NodeIndex + 1)]));
	const vec2 Next = (NodeIndex + 1 < N) ? m_vLineNodes[NodeIndex + 1] : (P + (P - m_vLineNodes[std::max(0, NodeIndex - 1)]));
	const vec2 Tangent = (Next - Prev) * 0.25f;
	m_vLineHandleIn[NodeIndex] = -Tangent;
	m_vLineHandleOut[NodeIndex] = Tangent;
}

int CEditor::LineToolHandleHitTest(float wx, float wy, int &OutKind) const
{
	OutKind = LINE_DRAG_NONE;
	const int N = (int)m_vLineNodes.size();
	const int Sel = m_LineSelectedNode;
	if(Sel < 0 || Sel >= N || (int)m_vLineHandleIn.size() != N || (int)m_vLineHandleOut.size() != N)
		return -1;
	// Only the currently-selected node exposes interactive handles.
	if(Sel < (int)m_vLineNodeKinds.size() && m_vLineNodeKinds[Sel] == LINE_NODE_SHARP)
		return -1;
	const float HitRange = 18.0f;
	int Best = -1;
	int BestKind = LINE_DRAG_NONE;
	float BestDistSq = HitRange * HitRange;
	const bool ClosedLoop = m_LineClosed && N >= 3;
	const bool HasIn = ClosedLoop || Sel > 0;
	const bool HasOut = ClosedLoop || Sel < N - 1;
	if(HasIn)
	{
		const vec2 H = m_vLineNodes[Sel] + m_vLineHandleIn[Sel];
		const float Dx = wx - H.x, Dy = wy - H.y;
		const float DSq = Dx * Dx + Dy * Dy;
		if(DSq < BestDistSq)
		{
			BestDistSq = DSq;
			Best = Sel;
			BestKind = LINE_DRAG_HANDLE_IN;
		}
	}
	if(HasOut)
	{
		const vec2 H = m_vLineNodes[Sel] + m_vLineHandleOut[Sel];
		const float Dx = wx - H.x, Dy = wy - H.y;
		const float DSq = Dx * Dx + Dy * Dy;
		if(DSq < BestDistSq)
		{
			BestDistSq = DSq;
			Best = Sel;
			BestKind = LINE_DRAG_HANDLE_OUT;
		}
	}
	OutKind = BestKind;
	return Best;
}

void CEditor::LineToolSamplePath(std::vector<vec2> &OutSamples, bool Closed) const
{
	OutSamples.clear();
	if(m_vLineNodes.empty())
		return;
	if(m_vLineNodes.size() == 1)
	{
		OutSamples.push_back(m_vLineNodes[0]);
		return;
	}
	const int N = (int)m_vLineNodes.size();
	const bool DoClose = Closed && N >= 3;
	const int Segments = DoClose ? N : (N - 1);
	const bool HaveHandles = (int)m_vLineHandleIn.size() == N && (int)m_vLineHandleOut.size() == N;
	const auto KindAt = [&](int Index) -> int {
		if(Index < 0 || Index >= (int)m_vLineNodeKinds.size())
			return LINE_NODE_SMOOTH;
		return m_vLineNodeKinds[Index];
	};
	for(int i = 0; i < Segments; ++i)
	{
		const int A = i;
		const int B = DoClose ? (i + 1) % N : (i + 1);
		vec2 P1 = m_vLineNodes[A];
		vec2 P2 = m_vLineNodes[B];
		const bool SegSharp = (KindAt(A) == LINE_NODE_SHARP) || (KindAt(B) == LINE_NODE_SHARP);

		const float Length = distance(P1, P2);
		int Samples = std::max(8, (int)(Length / 4.0f));
		if(i == 0)
			OutSamples.push_back(P1);

		if(m_LineCatmullRom)
		{
			vec2 P0, P3;
			if(DoClose)
			{
				P0 = m_vLineNodes[(A - 1 + N) % N];
				P3 = m_vLineNodes[(B + 1) % N];
			}
			else
			{
				P0 = (i > 0) ? m_vLineNodes[i - 1] : (P1 * 2.0f - P2);
				P3 = (i + 2 < N) ? m_vLineNodes[i + 2] : (P2 * 2.0f - P1);
			}
			for(int s = 1; s <= Samples; ++s)
			{
				float t = s / (float)Samples;
				OutSamples.push_back(CatmullRom(P0, P1, P2, P3, t));
			}
		}
		else if(SegSharp)
		{
			for(int s = 1; s <= Samples; ++s)
			{
				float t = s / (float)Samples;
				OutSamples.push_back(P1 + (P2 - P1) * t);
			}
		}
		else if(HaveHandles)
		{
			const vec2 H0 = P1 + m_vLineHandleOut[A];
			const vec2 H1 = P2 + m_vLineHandleIn[B];
			for(int s = 1; s <= Samples; ++s)
			{
				float t = s / (float)Samples;
				OutSamples.push_back(FadeToolCubicBezierAt(P1, H0, H1, P2, t));
			}
		}
		else
		{
			// Fallback (shouldn't fire in practice — handles are always seeded).
			vec2 P0 = (i > 0) ? m_vLineNodes[i - 1] : (P1 * 2.0f - P2);
			vec2 P3 = (i + 2 < N) ? m_vLineNodes[i + 2] : (P2 * 2.0f - P1);
			for(int s = 1; s <= Samples; ++s)
			{
				float t = s / (float)Samples;
				OutSamples.push_back(CatmullRom(P0, P1, P2, P3, t));
			}
		}
	}
}

int CEditor::LineToolFindHoveredNode(float wx, float wy) const
{
	const float HoverRadius = 26.0f;
	int Best = -1;
	float BestDistSq = HoverRadius * HoverRadius;
	for(int i = 0; i < (int)m_vLineNodes.size(); ++i)
	{
		float Dx = wx - m_vLineNodes[i].x;
		float Dy = wy - m_vLineNodes[i].y;
		float DSq = Dx * Dx + Dy * Dy;
		if(DSq < BestDistSq)
		{
			BestDistSq = DSq;
			Best = i;
		}
	}
	return Best;
}

int CEditor::LineToolFindHoveredSegment(float wx, float wy, vec2 &OutClosest) const
{
	if(m_vLineNodes.size() < 2)
		return -1;
	// Hit radius scales with thickness so the user can click anywhere on the rasterized cells.
	const float HoverRadius = std::max(20.0f, (float)m_LineThickness * 16.0f + 16.0f);
	float BestDistSq = HoverRadius * HoverRadius;
	int BestSeg = -1;
	const int N = (int)m_vLineNodes.size();
	const bool DoClose = m_LineClosed && N >= 3;
	const int Segments = DoClose ? N : (N - 1);
	const bool HaveHandles = (int)m_vLineHandleIn.size() == N && (int)m_vLineHandleOut.size() == N;
	const auto KindAt = [&](int Index) -> int {
		if(Index < 0 || Index >= (int)m_vLineNodeKinds.size())
			return LINE_NODE_SMOOTH;
		return m_vLineNodeKinds[Index];
	};
	for(int i = 0; i < Segments; ++i)
	{
		const int A = i;
		const int B = DoClose ? (i + 1) % N : (i + 1);
		vec2 P1 = m_vLineNodes[A];
		vec2 P2 = m_vLineNodes[B];
		const bool SegSharp = (KindAt(A) == LINE_NODE_SHARP) || (KindAt(B) == LINE_NODE_SHARP);
		const int Samples = std::max(8, (int)(distance(P1, P2) / 4.0f));
		for(int s = 0; s <= Samples; ++s)
		{
			float t = s / (float)Samples;
			vec2 Pt;
			if(m_LineCatmullRom)
			{
				vec2 P0 = (i > 0) ? m_vLineNodes[i - 1] : (P1 * 2.0f - P2);
				vec2 P3 = (i + 2 < N) ? m_vLineNodes[i + 2] : (P2 * 2.0f - P1);
				Pt = CatmullRom(P0, P1, P2, P3, t);
			}
			else if(SegSharp)
			{
				Pt = P1 + (P2 - P1) * t;
			}
			else if(HaveHandles)
			{
				const vec2 H0 = P1 + m_vLineHandleOut[A];
				const vec2 H1 = P2 + m_vLineHandleIn[B];
				Pt = FadeToolCubicBezierAt(P1, H0, H1, P2, t);
			}
			else
			{
				vec2 P0 = (i > 0) ? m_vLineNodes[i - 1] : (P1 * 2.0f - P2);
				vec2 P3 = (i + 2 < N) ? m_vLineNodes[i + 2] : (P2 * 2.0f - P1);
				Pt = CatmullRom(P0, P1, P2, P3, t);
			}
			float Dx = wx - Pt.x;
			float Dy = wy - Pt.y;
			float DSq = Dx * Dx + Dy * Dy;
			if(DSq < BestDistSq)
			{
				BestDistSq = DSq;
				BestSeg = i;
				OutClosest = Pt;
			}
		}
	}
	return BestSeg;
}

void CEditor::LineToolCollectCells(std::vector<std::pair<int, int>> &OutCells, bool Closed) const
{
	OutCells.clear();
	if(m_vLineNodes.size() < 2)
		return;
	std::vector<vec2> Samples;
	LineToolSamplePath(Samples, Closed);
	if(Samples.empty())
		return;

	std::set<std::pair<int, int>> CellSet;
	// Asymmetric range so even thicknesses also widen the line.
	// thickness 1 → range [0..0]   = 1 cell
	// thickness 2 → range [0..1]   = 2 cells
	// thickness 3 → range [-1..1]  = 3 cells
	// thickness 4 → range [-1..2]  = 4 cells, etc.
	const int Lower = (m_LineThickness - 1) / 2;
	const int Upper = m_LineThickness / 2;
	for(const vec2 &P : Samples)
	{
		const int Tx = (int)floorf(P.x / 32.0f);
		const int Ty = (int)floorf(P.y / 32.0f);
		for(int Dy = -Lower; Dy <= Upper; ++Dy)
			for(int Dx = -Lower; Dx <= Upper; ++Dx)
				CellSet.insert(std::make_pair(Tx + Dx, Ty + Dy));
	}
	OutCells.reserve(CellSet.size());
	for(const auto &Cell : CellSet)
		OutCells.push_back(Cell);
}

void CEditor::LineToolRenderAndInteract(float wx, float wy)
{
	if(m_LineActiveNode < 0)
	{
		// Handles take priority over node body so a dot near a node is still grabbable.
		int HoverKind = LINE_DRAG_NONE;
		int Hover = m_LineCatmullRom ? -1 : LineToolHandleHitTest(wx, wy, HoverKind);
		if(Hover < 0)
		{
			Hover = LineToolFindHoveredNode(wx, wy);
			HoverKind = (Hover >= 0) ? (int)LINE_DRAG_NODE : (int)LINE_DRAG_NONE;
		}
		m_LineHoverNode = Hover;
		m_LineHoverDragKind = HoverKind;
	}
	else
	{
		m_LineHoverNode = -1;
		m_LineHoverDragKind = LINE_DRAG_NONE;
	}

	// Real-time block preview: highlight the cells that would be placed.
	if(m_vLineNodes.size() >= 2)
	{
		const bool ShowClosed = m_LineClosed && (int)m_vLineNodes.size() >= 3;
		std::vector<std::pair<int, int>> PreviewCells;
		LineToolCollectCells(PreviewCells, ShowClosed);

		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 0.9f, 0.4f, 0.45f);
		for(const auto &Cell : PreviewCells)
		{
			IGraphics::CQuadItem QuadItem(Cell.first * 32.0f, Cell.second * 32.0f, 32.0f, 32.0f);
			Graphics()->QuadsDrawTL(&QuadItem, 1);
		}
		Graphics()->QuadsEnd();

		// Thin curve overlay so the path stays readable on top of the cell highlights.
		std::vector<vec2> Samples;
		LineToolSamplePath(Samples, ShowClosed);
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(1.0f, 0.7f, 0.0f, 0.9f);
		for(size_t i = 1; i < Samples.size(); ++i)
		{
			IGraphics::CLineItem L(Samples[i - 1].x, Samples[i - 1].y, Samples[i].x, Samples[i].y);
			Graphics()->LinesDraw(&L, 1);
		}
		Graphics()->LinesEnd();
	}

	const int N = (int)m_vLineNodes.size();
	const bool HaveHandles = (int)m_vLineHandleIn.size() == N && (int)m_vLineHandleOut.size() == N;
	const bool ClosedLoop = m_LineClosed && N >= 3;
	const auto KindAt = [&](int Index) -> int {
		if(Index < 0 || Index >= (int)m_vLineNodeKinds.size())
			return LINE_NODE_SMOOTH;
		return m_vLineNodeKinds[Index];
	};

	// Handle stems + dots — only for the selected node, only if it's smooth
	const int Sel = m_LineSelectedNode;
	const bool ShowHandles = !m_LineCatmullRom && HaveHandles && Sel >= 0 && Sel < N && KindAt(Sel) != LINE_NODE_SHARP;
	if(ShowHandles)
	{
		const vec2 &P = m_vLineNodes[Sel];
		const bool HasIn = ClosedLoop || Sel > 0;
		const bool HasOut = ClosedLoop || Sel < N - 1;

		// Stems
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(0.6f, 0.7f, 0.85f, 0.7f);
		if(HasIn)
		{
			const vec2 H = P + m_vLineHandleIn[Sel];
			IGraphics::CLineItem L(P.x, P.y, H.x, H.y);
			Graphics()->LinesDraw(&L, 1);
		}
		if(HasOut)
		{
			const vec2 H = P + m_vLineHandleOut[Sel];
			IGraphics::CLineItem L(P.x, P.y, H.x, H.y);
			Graphics()->LinesDraw(&L, 1);
		}
		Graphics()->LinesEnd();

		// Dots
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		if(HasIn)
		{
			const bool Active = (m_LineActiveNode == Sel && m_LineDragKind == LINE_DRAG_HANDLE_IN) || (m_LineHoverNode == Sel && m_LineHoverDragKind == LINE_DRAG_HANDLE_IN);
			if(Active)
				Graphics()->SetColor(1.0f, 0.55f, 0.1f, 1.0f);
			else
				Graphics()->SetColor(0.85f, 0.92f, 1.0f, 0.9f);
			const vec2 H = P + m_vLineHandleIn[Sel];
			DrawFilledCircle(Graphics(), H.x, H.y, Active ? 9.0f : 7.0f);
		}
		if(HasOut)
		{
			const bool Active = (m_LineActiveNode == Sel && m_LineDragKind == LINE_DRAG_HANDLE_OUT) || (m_LineHoverNode == Sel && m_LineHoverDragKind == LINE_DRAG_HANDLE_OUT);
			if(Active)
				Graphics()->SetColor(1.0f, 0.55f, 0.1f, 1.0f);
			else
				Graphics()->SetColor(0.85f, 0.92f, 1.0f, 0.9f);
			const vec2 H = P + m_vLineHandleOut[Sel];
			DrawFilledCircle(Graphics(), H.x, H.y, Active ? 9.0f : 7.0f);
		}
		Graphics()->QuadsEnd();
	}

	// Nodes: circle = smooth, square = sharp.
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	for(int i = 0; i < N; ++i)
	{
		const vec2 &P = m_vLineNodes[i];
		const bool NodeActive = (m_LineActiveNode == i && m_LineDragKind == LINE_DRAG_NODE);
		const bool NodeHover = (m_LineHoverNode == i && m_LineHoverDragKind == LINE_DRAG_NODE);
		const bool Highlighted = NodeActive || NodeHover;
		const float Size = Highlighted ? 28.0f : 20.0f;
		const int Kind = KindAt(i);
		if(Highlighted)
			Graphics()->SetColor(1.0f, 0.45f, 0.0f, 1.0f);
		else
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.95f);
		if(Kind == LINE_NODE_SHARP)
		{
			IGraphics::CQuadItem Q(P.x - Size * 0.5f, P.y - Size * 0.5f, Size, Size);
			Graphics()->QuadsDrawTL(&Q, 1);
		}
		else
		{
			DrawFilledCircle(Graphics(), P.x, P.y, Size * 0.5f);
		}
	}
	Graphics()->QuadsEnd();
}

void CEditor::LineToolCancel()
{
	m_vLineNodes.clear();
	m_vLineNodeKinds.clear();
	m_vLineHandleIn.clear();
	m_vLineHandleOut.clear();
	m_LineActiveNode = -1;
	m_LineHoverNode = -1;
	m_LineSelectedNode = -1;
	m_LineDragKind = LINE_DRAG_NONE;
	m_LineHoverDragKind = LINE_DRAG_NONE;
	m_LineClosed = false;
}

void CEditor::SelectToolSaveOrig()
{
	m_vSelectOrigTiles.clear();
	m_SelectOrigW = 0;
	m_SelectOrigH = 0;
	for(const auto &pLayer : m_pBrush->m_vpLayers)
	{
		if(pLayer->m_Type == LAYERTYPE_TILES)
		{
			auto pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
			m_SelectOrigW = pTiles->m_Width;
			m_SelectOrigH = pTiles->m_Height;
			m_vSelectOrigTiles.assign(pTiles->m_pTiles, pTiles->m_pTiles + pTiles->m_Width * pTiles->m_Height);
			break;
		}
	}
}

void CEditor::SelectToolCancelOrig()
{
	m_vSelectOrigTiles.clear();
	m_SelectOrigW = 0;
	m_SelectOrigH = 0;
	m_SelectTotalAngle = 0.0f;
	m_vSelectPath.clear();
}

// ── 3D perspective tool ───────────────────────────────────────────────────────

vec3 CEditor::ThreeDRotate(vec3 p, float rx, float ry, float rz)
{
	// Rotate around X axis
	float sinX = sinf(rx), cosX = cosf(rx);
	float y1 = p.y * cosX - p.z * sinX;
	float z1 = p.y * sinX + p.z * cosX;
	p.y = y1;
	p.z = z1;
	// Rotate around Y axis
	float sinY = sinf(ry), cosY = cosf(ry);
	float x2 = p.x * cosY + p.z * sinY;
	float z2 = -p.x * sinY + p.z * cosY;
	p.x = x2;
	p.z = z2;
	// Rotate around Z axis
	float sinZ = sinf(rz), cosZ = cosf(rz);
	float x3 = p.x * cosZ - p.y * sinZ;
	float y3 = p.x * sinZ + p.y * cosZ;
	p.x = x3;
	p.y = y3;
	return p;
}

vec2 CEditor::ThreeDProject(vec3 p, float perspPx)
{
	if(perspPx <= 0.0f)
		return vec2(p.x, p.y);
	const float Denom = perspPx + p.z;
	if(Denom < 1.0f)
		return vec2(p.x, p.y);
	const float Scale = perspPx / Denom;
	return vec2(p.x * Scale, p.y * Scale);
}

void CEditor::ThreeDCollectCells(int Shape, float Rx, float Ry, float Rz,
	float Sx, float Sy, float Sz, float PerspPx, float Cx, float Cy,
	int GridDiv, std::set<std::pair<int, int>> &Out)
{
	Out.clear();
	if(GridDiv < 1)
		GridDiv = 1;

	auto ProjFn = [&](vec3 p) -> vec2 {
		return ThreeDProject(ThreeDRotate(p, Rx, Ry, Rz), PerspPx) + vec2(Cx, Cy);
	};

	auto AddLine = [&](vec2 A, vec2 B) {
		int N = maximum(2, (int)ceilf(length(B - A) * 2.0f / 32.0f));
		for(int i = 0; i <= N; ++i)
		{
			float t = (float)i / N;
			Out.insert({(int)floorf((A.x + (B.x - A.x) * t) / 32.0f),
				(int)floorf((A.y + (B.y - A.y) * t) / 32.0f)});
		}
	};

	auto SegVis = [&](vec3 n) -> bool { return ThreeDRotate(n, Rx, Ry, Rz).z <= 0.0f; };

	if(Shape == THREED_CUBE)
	{
		const vec3 C8[8] = {
			{-Sx, -Sy, -Sz}, {Sx, -Sy, -Sz}, {Sx, Sy, -Sz}, {-Sx, Sy, -Sz},
			{-Sx, -Sy, Sz}, {Sx, -Sy, Sz}, {Sx, Sy, Sz}, {-Sx, Sy, Sz},
		};
		struct SFace
		{
			int v[4];
			vec3 n;
		};
		const SFace Faces[6] = {
			{{1, 5, 2, 6}, {1, 0, 0}},
			{{0, 4, 3, 7}, {-1, 0, 0}},
			{{3, 2, 7, 6}, {0, 1, 0}},
			{{0, 1, 4, 5}, {0, -1, 0}},
			{{4, 5, 7, 6}, {0, 0, 1}},
			{{0, 1, 3, 2}, {0, 0, -1}},
		};
		for(const auto &F : Faces)
		{
			if(!SegVis(F.n))
				continue;
			const vec3 &C00 = C8[F.v[0]], &C10 = C8[F.v[1]], &C01 = C8[F.v[2]], &C11 = C8[F.v[3]];
			for(int k = 0; k <= GridDiv; ++k)
			{
				float t = (float)k / GridDiv;
				AddLine(ProjFn(C00 + (C01 - C00) * t), ProjFn(C10 + (C11 - C10) * t));
				AddLine(ProjFn(C00 + (C10 - C00) * t), ProjFn(C01 + (C11 - C01) * t));
			}
		}
	}
	else if(Shape == THREED_SPHERE)
	{
		const int NumLat = GridDiv * 2;
		const int NumLon = maximum(4, GridDiv * 3);
		const int Segs = 64;
		for(int li = 1; li <= NumLat; ++li)
		{
			float phi = (float)li * pi / (NumLat + 1);
			float sp = sinf(phi), cp = cosf(phi);
			for(int j = 0; j < Segs; ++j)
			{
				float t0 = (float)j / Segs * 2.0f * pi;
				float t1 = (float)(j + 1) / Segs * 2.0f * pi;
				float tmid = (t0 + t1) * 0.5f;
				if(!SegVis({sp * cosf(tmid), cp, sp * sinf(tmid)}))
					continue;
				AddLine(ProjFn({Sx * sp * cosf(t0), Sy * cp, Sz * sp * sinf(t0)}),
					ProjFn({Sx * sp * cosf(t1), Sy * cp, Sz * sp * sinf(t1)}));
			}
		}
		for(int li = 0; li < NumLon; ++li)
		{
			float theta = (float)li / NumLon * 2.0f * pi;
			float ct = cosf(theta), st = sinf(theta);
			for(int j = 0; j < Segs; ++j)
			{
				float p0 = (float)j / Segs * pi;
				float p1 = (float)(j + 1) / Segs * pi;
				float pmid = (p0 + p1) * 0.5f;
				if(!SegVis({sinf(pmid) * ct, cosf(pmid), sinf(pmid) * st}))
					continue;
				AddLine(ProjFn({Sx * sinf(p0) * ct, Sy * cosf(p0), Sz * sinf(p0) * st}),
					ProjFn({Sx * sinf(p1) * ct, Sy * cosf(p1), Sz * sinf(p1) * st}));
			}
		}
	}
	else if(Shape == THREED_CYLINDER)
	{
		const int NumVLines = maximum(4, GridDiv * 4);
		const int Segs = 64;
		const bool TopVis = SegVis({0, 1, 0});
		const bool BotVis = SegVis({0, -1, 0});

		for(int ri = 0; ri <= GridDiv; ++ri)
		{
			float y = -Sy + (float)ri / GridDiv * 2.0f * Sy;
			for(int j = 0; j < Segs; ++j)
			{
				float t0 = (float)j / Segs * 2.0f * pi;
				float t1 = (float)(j + 1) / Segs * 2.0f * pi;
				float tmid = (t0 + t1) * 0.5f;
				if(!SegVis({cosf(tmid), 0, sinf(tmid)}))
					continue;
				AddLine(ProjFn({Sx * cosf(t0), y, Sz * sinf(t0)}),
					ProjFn({Sx * cosf(t1), y, Sz * sinf(t1)}));
			}
		}
		for(int li = 0; li < NumVLines; ++li)
		{
			float theta = (float)li / NumVLines * 2.0f * pi;
			if(!SegVis({cosf(theta), 0, sinf(theta)}))
				continue;
			AddLine(ProjFn({Sx * cosf(theta), -Sy, Sz * sinf(theta)}),
				ProjFn({Sx * cosf(theta), Sy, Sz * sinf(theta)}));
		}
		// Cap cross lines on visible end faces
		const int NumCapLines = maximum(4, GridDiv * 2);
		for(int side = 0; side < 2; ++side)
		{
			if(side == 0 && !TopVis)
				continue;
			if(side == 1 && !BotVis)
				continue;
			float CapY = (side == 0) ? Sy : -Sy;
			for(int li = 0; li < NumCapLines; ++li)
			{
				float theta = (float)li / NumCapLines * pi; // half-turn: each line is a full diameter
				AddLine(ProjFn({-Sx * cosf(theta), CapY, -Sz * sinf(theta)}),
					ProjFn({Sx * cosf(theta), CapY, Sz * sinf(theta)}));
			}
		}
	}
	else if(Shape == THREED_TORUS)
	{
		const float R = Sx, r = Sy;
		const float ZScale = (Sx > 0.0f) ? (Sz / Sx) : 1.0f;
		const int NumTube = maximum(4, GridDiv * 4);
		const int NumRing = maximum(4, GridDiv * 4);
		const int Segs = 64;
		for(int ti = 0; ti < NumTube; ++ti)
		{
			float theta = (float)ti / NumTube * 2.0f * pi;
			float ct = cosf(theta), st = sinf(theta);
			for(int j = 0; j < Segs; ++j)
			{
				float p0 = (float)j / Segs * 2.0f * pi;
				float p1 = (float)(j + 1) / Segs * 2.0f * pi;
				float pmid = (p0 + p1) * 0.5f;
				if(!SegVis({cosf(pmid) * ct, sinf(pmid), cosf(pmid) * st}))
					continue;
				AddLine(ProjFn({(R + r * cosf(p0)) * ct, r * sinf(p0), (R + r * cosf(p0)) * st * ZScale}),
					ProjFn({(R + r * cosf(p1)) * ct, r * sinf(p1), (R + r * cosf(p1)) * st * ZScale}));
			}
		}
		for(int ri = 0; ri < NumRing; ++ri)
		{
			float phi = (float)ri / NumRing * 2.0f * pi;
			float cp = cosf(phi), sp = sinf(phi);
			for(int j = 0; j < Segs; ++j)
			{
				float t0 = (float)j / Segs * 2.0f * pi;
				float t1 = (float)(j + 1) / Segs * 2.0f * pi;
				float tmid = (t0 + t1) * 0.5f;
				if(!SegVis({cp * cosf(tmid), sp, cp * sinf(tmid)}))
					continue;
				AddLine(ProjFn({(R + r * cp) * cosf(t0), r * sp, (R + r * cp) * sinf(t0) * ZScale}),
					ProjFn({(R + r * cp) * cosf(t1), r * sp, (R + r * cp) * sinf(t1) * ZScale}));
			}
		}
	}
}

void CEditor::ThreeDToolEnsureInit()
{
	if(m_ThreeDInit)
		return;
	m_ThreeDCenter = vec2(MapView()->GetWorldOffset().x, MapView()->GetWorldOffset().y);
	m_ThreeDScaleX = 3.0f * 32.0f;
	m_ThreeDScaleY = 3.0f * 32.0f;
	m_ThreeDScaleZ = 3.0f * 32.0f;
	m_ThreeDRxDeg = 20.0f;
	m_ThreeDRyDeg = 30.0f;
	m_ThreeDRzDeg = 0.0f;
	m_ThreeDPerspTiles = 20.0f;
	m_ThreeDDragNode = -1;
	m_ThreeDHoverNode = -1;
	m_ThreeDInit = true;
}

void CEditor::ThreeDToolCancel()
{
	m_ThreeDInit = false;
	m_ThreeDDragNode = -1;
	m_ThreeDHoverNode = -1;
}

void CEditor::ThreeDToolCommit()
{
	if(m_pBrush->IsEmpty())
		return;

	const float Rx = m_ThreeDRxDeg * pi / 180.0f;
	const float Ry = m_ThreeDRyDeg * pi / 180.0f;
	const float Rz = m_ThreeDRzDeg * pi / 180.0f;
	const float PerspPx = m_ThreeDPerspTiles * 32.0f;

	std::set<std::pair<int, int>> CellSet;
	ThreeDCollectCells(m_ThreeDShape, Rx, Ry, Rz, m_ThreeDScaleX, m_ThreeDScaleY, m_ThreeDScaleZ, PerspPx, m_ThreeDCenter.x, m_ThreeDCenter.y, m_ThreeDGridDiv, CellSet);
	if(CellSet.empty())
		return;

	std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
	if(!pGroup)
		return;

	for(int LayerIdx : Map()->m_vSelectedLayers)
	{
		if(LayerIdx < 0 || LayerIdx >= (int)pGroup->m_vpLayers.size())
			continue;
		std::shared_ptr<CLayer> pLayer = pGroup->m_vpLayers[LayerIdx];
		if(pLayer->m_Type != LAYERTYPE_TILES || pLayer->m_Readonly)
			continue;
		auto pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);

		for(const auto &pBrushLayer : m_pBrush->m_vpLayers)
		{
			if(pBrushLayer->m_Type != LAYERTYPE_TILES)
				continue;
			auto pBrushTiles = std::static_pointer_cast<CLayerTiles>(pBrushLayer);
			if(pBrushTiles->m_Width == 0 || pBrushTiles->m_Height == 0)
				continue;
			if((!pTiles->m_HasTele || pBrushTiles->m_HasTele) &&
				(!pTiles->m_HasSpeedup || pBrushTiles->m_HasSpeedup) &&
				(!pTiles->m_HasFront || pBrushTiles->m_HasFront) &&
				(!pTiles->m_HasGame || pBrushTiles->m_HasGame) &&
				(!pTiles->m_HasSwitch || pBrushTiles->m_HasSwitch) &&
				(!pTiles->m_HasTune || pBrushTiles->m_HasTune))
			{
				const CTile BrushTile = pBrushTiles->m_pTiles[0];
				const int OffTileX = pGroup->m_OffsetX / 32;
				const int OffTileY = pGroup->m_OffsetY / 32;
				for(const auto &Cell : CellSet)
				{
					const int Tx = Cell.first - OffTileX;
					const int Ty = Cell.second - OffTileY;
					if(Tx >= 0 && Tx < pTiles->m_Width && Ty >= 0 && Ty < pTiles->m_Height)
						pTiles->SetTile(Tx, Ty, BrushTile);
				}
			}
			break;
		}
	}

	std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
	if(!pAction->IsEmpty())
		Map()->m_EditorHistory.RecordAction(pAction);
}

int CEditor::ThreeDToolHitNode(float wx, float wy) const
{
	if(!m_ThreeDInit)
		return -1;
	const float Rx = m_ThreeDRxDeg * pi / 180.0f;
	const float Ry = m_ThreeDRyDeg * pi / 180.0f;
	const float Rz = m_ThreeDRzDeg * pi / 180.0f;
	const float PerspPx = m_ThreeDPerspTiles * 32.0f;
	const vec2 Nodes[5] = {
		m_ThreeDCenter,
		ThreeDProject(ThreeDRotate({m_ThreeDScaleX, 0, 0}, Rx, Ry, Rz), PerspPx) + m_ThreeDCenter,
		ThreeDProject(ThreeDRotate({0, m_ThreeDScaleY, 0}, Rx, Ry, Rz), PerspPx) + m_ThreeDCenter,
		ThreeDProject(ThreeDRotate({0, 0, m_ThreeDScaleZ}, Rx, Ry, Rz), PerspPx) + m_ThreeDCenter,
		m_ThreeDCenter + vec2(0.0f, -5.0f * 32.0f),
	};
	const float HitRSq = (16.0f * m_MouseWorldScale) * (16.0f * m_MouseWorldScale);
	for(int i = 0; i < 5; ++i)
	{
		float Dx = wx - Nodes[i].x;
		float Dy = wy - Nodes[i].y;
		if(Dx * Dx + Dy * Dy <= HitRSq)
			return i;
	}
	return -1;
}

void CEditor::ThreeDToolRenderAndInteract(float wx, float wy, float mx, float my)
{
	if(!m_ThreeDInit)
		return;

	const float Rx = m_ThreeDRxDeg * pi / 180.0f;
	const float Ry = m_ThreeDRyDeg * pi / 180.0f;
	const float Rz = m_ThreeDRzDeg * pi / 180.0f;
	const float PerspPx = m_ThreeDPerspTiles * 32.0f;

	if(m_ThreeDDragNode == -1)
		m_ThreeDHoverNode = ThreeDToolHitNode(wx, wy);
	else
		m_ThreeDHoverNode = -1;

	// Project helper (world-space result, includes center offset)
	auto Proj = [&](vec3 p) -> vec2 {
		return ThreeDProject(ThreeDRotate(p, Rx, Ry, Rz), PerspPx) + m_ThreeDCenter;
	};
	auto DrawLine = [&](vec2 A, vec2 B) {
		IGraphics::CLineItem L(A.x, A.y, B.x, B.y);
		Graphics()->LinesDraw(&L, 1);
	};

	// Ghost tile blocks
	if(m_ThreeDRenderMode == THREED_RENDER_GHOST || m_ThreeDRenderMode == THREED_RENDER_BOTH)
	{
		std::set<std::pair<int, int>> CellSet;
		ThreeDCollectCells(m_ThreeDShape, Rx, Ry, Rz, m_ThreeDScaleX, m_ThreeDScaleY, m_ThreeDScaleZ, PerspPx, m_ThreeDCenter.x, m_ThreeDCenter.y, m_ThreeDGridDiv, CellSet);
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.35f);
		for(const auto &C : CellSet)
		{
			IGraphics::CQuadItem Q(C.first * 32.0f, C.second * 32.0f, 32.0f, 32.0f);
			Graphics()->QuadsDrawTL(&Q, 1);
		}
		Graphics()->QuadsEnd();
	}

	// Wireframe overlay (back-face culled, GridDiv-aware to match tile stamp pattern)
	if(m_ThreeDRenderMode == THREED_RENDER_WIRE || m_ThreeDRenderMode == THREED_RENDER_BOTH)
	{
		const float Sx = m_ThreeDScaleX, Sy = m_ThreeDScaleY, Sz = m_ThreeDScaleZ;
		const int GridDiv = m_ThreeDGridDiv;
		const int Segs = maximum(32, GridDiv * 8);

		auto SegVis = [&](vec3 n) -> bool { return ThreeDRotate(n, Rx, Ry, Rz).z <= 0.0f; };

		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.75f);

		if(m_ThreeDShape == THREED_CUBE)
		{
			const vec3 C8[8] = {
				{-Sx, -Sy, -Sz}, {Sx, -Sy, -Sz}, {Sx, Sy, -Sz}, {-Sx, Sy, -Sz},
				{-Sx, -Sy, Sz}, {Sx, -Sy, Sz}, {Sx, Sy, Sz}, {-Sx, Sy, Sz},
			};
			struct SFace
			{
				int v[4];
				vec3 n;
			};
			const SFace Faces[6] = {
				{{1, 5, 2, 6}, {1, 0, 0}},
				{{0, 4, 3, 7}, {-1, 0, 0}},
				{{3, 2, 7, 6}, {0, 1, 0}},
				{{0, 1, 4, 5}, {0, -1, 0}},
				{{4, 5, 7, 6}, {0, 0, 1}},
				{{0, 1, 3, 2}, {0, 0, -1}},
			};
			for(const auto &F : Faces)
			{
				if(!SegVis(F.n))
					continue;
				const vec3 &C00 = C8[F.v[0]], &C10 = C8[F.v[1]], &C01 = C8[F.v[2]], &C11 = C8[F.v[3]];
				for(int k = 0; k <= GridDiv; ++k)
				{
					float t = (float)k / GridDiv;
					DrawLine(Proj(C00 + (C01 - C00) * t), Proj(C10 + (C11 - C10) * t));
					DrawLine(Proj(C00 + (C10 - C00) * t), Proj(C01 + (C11 - C01) * t));
				}
			}
		}
		else if(m_ThreeDShape == THREED_SPHERE)
		{
			const int NumLat = GridDiv * 2;
			const int NumLon = maximum(4, GridDiv * 3);
			for(int li = 1; li <= NumLat; ++li)
			{
				float phi = (float)li * pi / (NumLat + 1);
				float sp = sinf(phi), cp = cosf(phi);
				for(int j = 0; j < Segs; ++j)
				{
					float t0 = (float)j / Segs * 2.0f * pi;
					float t1 = (float)(j + 1) / Segs * 2.0f * pi;
					float tmid = (t0 + t1) * 0.5f;
					if(!SegVis({sp * cosf(tmid), cp, sp * sinf(tmid)}))
						continue;
					DrawLine(Proj({Sx * sp * cosf(t0), Sy * cp, Sz * sp * sinf(t0)}),
						Proj({Sx * sp * cosf(t1), Sy * cp, Sz * sp * sinf(t1)}));
				}
			}
			for(int li = 0; li < NumLon; ++li)
			{
				float theta = (float)li / NumLon * 2.0f * pi;
				float ct = cosf(theta), st = sinf(theta);
				for(int j = 0; j < Segs; ++j)
				{
					float p0 = (float)j / Segs * pi;
					float p1 = (float)(j + 1) / Segs * pi;
					float pmid = (p0 + p1) * 0.5f;
					if(!SegVis({sinf(pmid) * ct, cosf(pmid), sinf(pmid) * st}))
						continue;
					DrawLine(Proj({Sx * sinf(p0) * ct, Sy * cosf(p0), Sz * sinf(p0) * st}),
						Proj({Sx * sinf(p1) * ct, Sy * cosf(p1), Sz * sinf(p1) * st}));
				}
			}
		}
		else if(m_ThreeDShape == THREED_CYLINDER)
		{
			const int NumVLines = maximum(4, GridDiv * 4);
			const bool TopVis = SegVis({0, 1, 0});
			const bool BotVis = SegVis({0, -1, 0});
			for(int ri = 0; ri <= GridDiv; ++ri)
			{
				float y = -Sy + (float)ri / GridDiv * 2.0f * Sy;
				for(int j = 0; j < Segs; ++j)
				{
					float t0 = (float)j / Segs * 2.0f * pi;
					float t1 = (float)(j + 1) / Segs * 2.0f * pi;
					float tmid = (t0 + t1) * 0.5f;
					if(!SegVis({cosf(tmid), 0, sinf(tmid)}))
						continue;
					DrawLine(Proj({Sx * cosf(t0), y, Sz * sinf(t0)}),
						Proj({Sx * cosf(t1), y, Sz * sinf(t1)}));
				}
			}
			for(int li = 0; li < NumVLines; ++li)
			{
				float theta = (float)li / NumVLines * 2.0f * pi;
				if(!SegVis({cosf(theta), 0, sinf(theta)}))
					continue;
				DrawLine(Proj({Sx * cosf(theta), -Sy, Sz * sinf(theta)}),
					Proj({Sx * cosf(theta), Sy, Sz * sinf(theta)}));
			}
			// Cap diameter lines on visible end faces
			const int NumCapLines = maximum(4, GridDiv * 2);
			for(int side = 0; side < 2; ++side)
			{
				if(side == 0 && !TopVis)
					continue;
				if(side == 1 && !BotVis)
					continue;
				float CapY = (side == 0) ? Sy : -Sy;
				for(int li = 0; li < NumCapLines; ++li)
				{
					float theta = (float)li / NumCapLines * pi;
					DrawLine(Proj({-Sx * cosf(theta), CapY, -Sz * sinf(theta)}),
						Proj({Sx * cosf(theta), CapY, Sz * sinf(theta)}));
				}
			}
		}
		else if(m_ThreeDShape == THREED_TORUS)
		{
			const float R = Sx, r = Sy;
			const float ZScale = (Sx > 0.0f) ? (Sz / Sx) : 1.0f;
			const int NumTube = maximum(4, GridDiv * 4);
			const int NumRing = maximum(4, GridDiv * 4);
			for(int ti = 0; ti < NumTube; ++ti)
			{
				float theta = (float)ti / NumTube * 2.0f * pi;
				float ct = cosf(theta), st = sinf(theta);
				for(int j = 0; j < Segs; ++j)
				{
					float p0 = (float)j / Segs * 2.0f * pi;
					float p1 = (float)(j + 1) / Segs * 2.0f * pi;
					float pmid = (p0 + p1) * 0.5f;
					if(!SegVis({cosf(pmid) * ct, sinf(pmid), cosf(pmid) * st}))
						continue;
					DrawLine(Proj({(R + r * cosf(p0)) * ct, r * sinf(p0), (R + r * cosf(p0)) * st * ZScale}),
						Proj({(R + r * cosf(p1)) * ct, r * sinf(p1), (R + r * cosf(p1)) * st * ZScale}));
				}
			}
			for(int ri = 0; ri < NumRing; ++ri)
			{
				float phi = (float)ri / NumRing * 2.0f * pi;
				float cp = cosf(phi), sp = sinf(phi);
				for(int j = 0; j < Segs; ++j)
				{
					float t0 = (float)j / Segs * 2.0f * pi;
					float t1 = (float)(j + 1) / Segs * 2.0f * pi;
					float tmid = (t0 + t1) * 0.5f;
					if(!SegVis({cp * cosf(tmid), sp, cp * sinf(tmid)}))
						continue;
					DrawLine(Proj({(R + r * cp) * cosf(t0), r * sp, (R + r * cp) * sinf(t0) * ZScale}),
						Proj({(R + r * cp) * cosf(t1), r * sp, (R + r * cp) * sinf(t1) * ZScale}));
				}
			}
		}

		Graphics()->LinesEnd();
	}

	// Node world positions
	const vec2 Nodes[5] = {
		m_ThreeDCenter,
		Proj({m_ThreeDScaleX, 0, 0}),
		Proj({0, m_ThreeDScaleY, 0}),
		Proj({0, 0, m_ThreeDScaleZ}),
		m_ThreeDCenter + vec2(0.0f, -5.0f * 32.0f),
	};
	const ColorRGBA NodeColors[5] = {
		{1.0f, 1.0f, 0.2f, 1.0f}, // center: yellow
		{1.0f, 0.2f, 0.2f, 1.0f}, // X scale: red
		{0.2f, 1.0f, 0.2f, 1.0f}, // Y scale: green
		{0.2f, 0.4f, 1.0f, 1.0f}, // Z scale: blue
		{0.2f, 1.0f, 1.0f, 1.0f}, // rotation: cyan
	};

	// Axis lines center -> scale nodes
	Graphics()->TextureClear();
	Graphics()->LinesBegin();
	for(int i = 1; i <= 3; ++i)
	{
		const ColorRGBA &C = NodeColors[i];
		Graphics()->SetColor(C.r, C.g, C.b, 0.5f);
		DrawLine(Nodes[0], Nodes[i]);
	}
	Graphics()->LinesEnd();

	// Node handles
	const float NodeSz = maximum(6.0f, 10.0f * m_MouseWorldScale);
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	for(int i = 0; i < 5; ++i)
	{
		const bool Hot = (m_ThreeDDragNode == i) || (m_ThreeDDragNode == -1 && m_ThreeDHoverNode == i);
		ColorRGBA Col = NodeColors[i];
		Col.a = Hot ? 1.0f : 0.7f;
		Graphics()->SetColor(Col);
		const float Sz = Hot ? NodeSz * 1.3f : NodeSz;
		IGraphics::CQuadItem Q(Nodes[i].x - Sz * 0.5f, Nodes[i].y - Sz * 0.5f, Sz, Sz);
		Graphics()->QuadsDrawTL(&Q, 1);
	}
	Graphics()->QuadsEnd();

	// Label
	Ui()->MapScreen();
	{
		const char *apShapeNames[] = {"Cube", "Sphere", "Cylinder", "Torus"};
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "%s  Rx:%.0f Ry:%.0f Rz:%.0f  Cyan=rotate  Enter stamp",
			apShapeNames[m_ThreeDShape], m_ThreeDRxDeg, m_ThreeDRyDeg, m_ThreeDRzDeg);
		TextRender()->Text(mx + 12.0f, my - 14.0f, 10.0f, aBuf, -1.0f);
	}
}

// ── Shape ghost ──────────────────────────────────────────────────────────────

void CEditor::ShapeGhostClear()
{
	m_ShapeGhostInit = false;
	m_ShapeGhostDragNode = -1;
	m_ShapeGhostHoverNode = -1;
}

void CEditor::ShapeGhostCommit()
{
	if(!m_ShapeGhostInit || m_pBrush->IsEmpty())
		return;

	std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
	if(!pGroup)
		return;

	for(int LayerIdx : Map()->m_vSelectedLayers)
	{
		if(LayerIdx < 0 || LayerIdx >= (int)pGroup->m_vpLayers.size())
			continue;
		std::shared_ptr<CLayer> pLayer = pGroup->m_vpLayers[LayerIdx];
		if(pLayer->m_Type != LAYERTYPE_TILES || pLayer->m_Readonly)
			continue;
		for(const auto &pBrushLayer : m_pBrush->m_vpLayers)
		{
			if(pBrushLayer->m_Type != LAYERTYPE_TILES)
				continue;
			auto pTL = std::static_pointer_cast<CLayerTiles>(pLayer);
			auto pBL = std::static_pointer_cast<CLayerTiles>(pBrushLayer);
			if((!pTL->m_HasTele || pBL->m_HasTele) &&
				(!pTL->m_HasSpeedup || pBL->m_HasSpeedup) &&
				(!pTL->m_HasFront || pBL->m_HasFront) &&
				(!pTL->m_HasGame || pBL->m_HasGame) &&
				(!pTL->m_HasSwitch || pBL->m_HasSwitch) &&
				(!pTL->m_HasTune || pBL->m_HasTune))
			{
				pTL->BrushShape(pBrushLayer.get(),
					m_ShapeGhostX1, m_ShapeGhostY1,
					m_ShapeGhostX2, m_ShapeGhostY2,
					m_ShapeKind, m_ShapeFilled, m_ShapeThickness, m_ShapeNgonSides);
			}
			break;
		}
	}
	std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
	if(!pAction->IsEmpty())
		Map()->m_EditorHistory.RecordAction(pAction);
	ShapeGhostClear();
}

int CEditor::ShapeGhostHitNode(float wx, float wy) const
{
	if(!m_ShapeGhostInit)
		return -1;
	const float cx = (m_ShapeGhostX1 + m_ShapeGhostX2 + 1) * 16.0f;
	const float cy = (m_ShapeGhostY1 + m_ShapeGhostY2 + 1) * 16.0f;
	const vec2 Nodes[5] = {
		{cx, cy},
		{(float)m_ShapeGhostX1 * 32.0f, (float)m_ShapeGhostY1 * 32.0f},
		{(float)(m_ShapeGhostX2 + 1) * 32.0f, (float)m_ShapeGhostY1 * 32.0f},
		{(float)m_ShapeGhostX1 * 32.0f, (float)(m_ShapeGhostY2 + 1) * 32.0f},
		{(float)(m_ShapeGhostX2 + 1) * 32.0f, (float)(m_ShapeGhostY2 + 1) * 32.0f},
	};
	const float HitRSq = (14.0f * m_MouseWorldScale) * (14.0f * m_MouseWorldScale);
	for(int i = 0; i < 5; ++i)
	{
		float Dx = wx - Nodes[i].x;
		float Dy = wy - Nodes[i].y;
		if(Dx * Dx + Dy * Dy <= HitRSq)
			return i;
	}
	return -1;
}

void CEditor::ShapeGhostRenderAndInteract(float wx, float wy, float mx, float my)
{
	if(!m_ShapeGhostInit)
		return;

	// Update hover
	if(m_ShapeGhostDragNode == -1)
		m_ShapeGhostHoverNode = ShapeGhostHitNode(wx, wy);
	else
		m_ShapeGhostHoverNode = -1;

	// Ghost preview — same cells the stamp would write
	{
		std::vector<std::pair<int, int>> Cells;
		CLayerTiles::CollectShapeCells(m_ShapeGhostX1, m_ShapeGhostY1, m_ShapeGhostX2, m_ShapeGhostY2,
			m_ShapeKind, m_ShapeFilled, m_ShapeThickness, Cells, m_ShapeNgonSides);
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.35f);
		for(const auto &C : Cells)
		{
			IGraphics::CQuadItem Q(C.first * 32.0f, C.second * 32.0f, 32.0f, 32.0f);
			Graphics()->QuadsDrawTL(&Q, 1);
		}
		Graphics()->QuadsEnd();
	}

	// Node handles: 0=center(yellow), 1-4=corners(green)
	const float cx = (m_ShapeGhostX1 + m_ShapeGhostX2 + 1) * 16.0f;
	const float cy = (m_ShapeGhostY1 + m_ShapeGhostY2 + 1) * 16.0f;
	const vec2 Nodes[5] = {
		{cx, cy},
		{(float)m_ShapeGhostX1 * 32.0f, (float)m_ShapeGhostY1 * 32.0f},
		{(float)(m_ShapeGhostX2 + 1) * 32.0f, (float)m_ShapeGhostY1 * 32.0f},
		{(float)m_ShapeGhostX1 * 32.0f, (float)(m_ShapeGhostY2 + 1) * 32.0f},
		{(float)(m_ShapeGhostX2 + 1) * 32.0f, (float)(m_ShapeGhostY2 + 1) * 32.0f},
	};
	const ColorRGBA NodeColors[5] = {
		{1.0f, 1.0f, 0.2f, 1.0f},
		{0.2f, 1.0f, 0.2f, 1.0f},
		{0.2f, 1.0f, 0.2f, 1.0f},
		{0.2f, 1.0f, 0.2f, 1.0f},
		{0.2f, 1.0f, 0.2f, 1.0f},
	};
	const float NodeSz = maximum(6.0f, 10.0f * m_MouseWorldScale);
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	for(int i = 0; i < 5; ++i)
	{
		const bool Hot = (m_ShapeGhostDragNode == i) || (m_ShapeGhostDragNode == -1 && m_ShapeGhostHoverNode == i);
		ColorRGBA Col = NodeColors[i];
		Col.a = Hot ? 1.0f : 0.7f;
		Graphics()->SetColor(Col);
		const float Sz = Hot ? NodeSz * 1.3f : NodeSz;
		IGraphics::CQuadItem Q(Nodes[i].x - Sz * 0.5f, Nodes[i].y - Sz * 0.5f, Sz, Sz);
		Graphics()->QuadsDrawTL(&Q, 1);
	}
	Graphics()->QuadsEnd();

	// Label
	Ui()->MapScreen();
	{
		const int W = m_ShapeGhostX2 - m_ShapeGhostX1 + 1;
		const int H = m_ShapeGhostY2 - m_ShapeGhostY1 + 1;
		char aBuf[32];
		str_format(aBuf, sizeof(aBuf), "%d\xc3\x97%d  Enter to stamp", W, H);
		TextRender()->Text(mx + 12.0f, my - 14.0f, 10.0f, aBuf, -1.0f);
	}
}

void CEditor::LineToolCommit(bool Closed)
{
	if(m_vLineNodes.size() < 2)
	{
		LineToolCancel();
		return;
	}
	// No-op if no tile is selected — keep nodes intact so the user can pick a tile and try again.
	if(m_pBrush->IsEmpty())
		return;

	std::vector<std::pair<int, int>> Cells;
	LineToolCollectCells(Cells, Closed);
	if(Cells.empty())
	{
		LineToolCancel();
		return;
	}

	const bool SavedDestructive = m_BrushDrawDestructive;
	m_BrushDrawDestructive = true;

	for(int LayerIdx : Map()->m_vSelectedLayers)
	{
		if(LayerIdx < 0 || LayerIdx >= (int)Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers.size())
			continue;
		std::shared_ptr<CLayer> pLayer = Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers[LayerIdx];
		if(pLayer->m_Type != LAYERTYPE_TILES)
			continue;
		std::shared_ptr<CLayerTiles> pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);

		std::shared_ptr<CLayer> pBrushChosen;
		for(const auto &pBrushLayer : m_pBrush->m_vpLayers)
		{
			if(pBrushLayer->m_Type != LAYERTYPE_TILES)
				continue;
			auto pBrushTiles = std::static_pointer_cast<CLayerTiles>(pBrushLayer);
			if(pBrushTiles->m_Width != 1 || pBrushTiles->m_Height != 1)
				continue;
			if((!pTiles->m_HasTele || pBrushTiles->m_HasTele) && (!pTiles->m_HasSpeedup || pBrushTiles->m_HasSpeedup) && (!pTiles->m_HasFront || pBrushTiles->m_HasFront) && (!pTiles->m_HasGame || pBrushTiles->m_HasGame) && (!pTiles->m_HasSwitch || pBrushTiles->m_HasSwitch) && (!pTiles->m_HasTune || pBrushTiles->m_HasTune))
			{
				pBrushChosen = pBrushLayer;
				break;
			}
		}
		if(!pBrushChosen)
			continue;

		for(const auto &Cell : Cells)
		{
			if(Cell.first < 0 || Cell.first >= pTiles->m_Width || Cell.second < 0 || Cell.second >= pTiles->m_Height)
				continue;
			pTiles->BrushDraw(pBrushChosen.get(), vec2(Cell.first * 32.0f + 16.0f, Cell.second * 32.0f + 16.0f));
		}
	}

	m_BrushDrawDestructive = SavedDestructive;

	std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
	if(!pAction->IsEmpty())
		Map()->m_EditorHistory.RecordAction(pAction);

	LineToolCancel();
}

void CEditor::RecentTilesSnapshot()
{
	if(m_pBrush->IsEmpty())
		return;
	std::shared_ptr<CLayerTiles> pTiles;
	for(const auto &pLayer : m_pBrush->m_vpLayers)
	{
		if(pLayer->m_Type == LAYERTYPE_TILES)
		{
			auto pCand = std::static_pointer_cast<CLayerTiles>(pLayer);
			if(pCand->m_Width >= 1 && pCand->m_Height >= 1)
			{
				pTiles = pCand;
				break;
			}
		}
	}
	if(!pTiles)
		return;
	const CTile T = pTiles->m_pTiles[0];
	if(T.m_Index == 0)
		return;

	CRecentTile Entry;
	Entry.m_Tile = T;
	Entry.m_ImageId = pTiles->m_Image;
	Entry.m_HasGame = pTiles->m_HasGame;
	Entry.m_HasFront = pTiles->m_HasFront;
	Entry.m_HasTele = pTiles->m_HasTele;
	Entry.m_HasSwitch = pTiles->m_HasSwitch;
	Entry.m_HasSpeedup = pTiles->m_HasSpeedup;
	Entry.m_HasTune = pTiles->m_HasTune;

	const auto Same = [&](const CRecentTile &A, const CRecentTile &B) {
		return A.m_Tile.m_Index == B.m_Tile.m_Index && A.m_Tile.m_Flags == B.m_Tile.m_Flags &&
		       A.m_ImageId == B.m_ImageId && A.m_HasGame == B.m_HasGame && A.m_HasFront == B.m_HasFront &&
		       A.m_HasTele == B.m_HasTele && A.m_HasSwitch == B.m_HasSwitch &&
		       A.m_HasSpeedup == B.m_HasSpeedup && A.m_HasTune == B.m_HasTune;
	};
	for(auto It = m_vRecentTiles.begin(); It != m_vRecentTiles.end(); ++It)
	{
		if(Same(*It, Entry))
		{
			m_vRecentTiles.erase(It);
			break;
		}
	}
	m_vRecentTiles.insert(m_vRecentTiles.begin(), Entry);
	const size_t Cap = 10;
	if(m_vRecentTiles.size() > Cap)
		m_vRecentTiles.resize(Cap);
}

void CEditor::RecentTilesUse(int Index)
{
	if(Index < 0 || Index >= (int)m_vRecentTiles.size())
		return;
	const CRecentTile R = m_vRecentTiles[Index];

	// Try to switch the selection to a layer in the current group that matches the tile's
	// origin (entity flags + image), so the brush makes sense in context.
	std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
	if(pGroup)
	{
		for(size_t i = 0; i < pGroup->m_vpLayers.size(); ++i)
		{
			std::shared_ptr<CLayer> pLayer = pGroup->m_vpLayers[i];
			if(pLayer->m_Type != LAYERTYPE_TILES)
				continue;
			auto pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
			if(pTiles->m_HasGame == R.m_HasGame && pTiles->m_HasFront == R.m_HasFront &&
				pTiles->m_HasTele == R.m_HasTele && pTiles->m_HasSwitch == R.m_HasSwitch &&
				pTiles->m_HasSpeedup == R.m_HasSpeedup && pTiles->m_HasTune == R.m_HasTune &&
				pTiles->m_Image == R.m_ImageId)
			{
				Map()->m_vSelectedLayers.clear();
				Map()->m_vSelectedLayers.push_back((int)i);
				break;
			}
		}
	}

	// Build a 1x1 brush of the matching derived type so tele/switch/etc. metadata flows through.
	std::shared_ptr<CLayerTiles> pBrushLayer;
	if(R.m_HasTele)
		pBrushLayer = std::make_shared<CLayerTele>(Map(), 1, 1);
	else if(R.m_HasSwitch)
		pBrushLayer = std::make_shared<CLayerSwitch>(Map(), 1, 1);
	else if(R.m_HasSpeedup)
		pBrushLayer = std::make_shared<CLayerSpeedup>(Map(), 1, 1);
	else if(R.m_HasTune)
		pBrushLayer = std::make_shared<CLayerTune>(Map(), 1, 1);
	else
		pBrushLayer = std::make_shared<CLayerTiles>(Map(), 1, 1);

	pBrushLayer->m_HasGame = R.m_HasGame;
	pBrushLayer->m_HasFront = R.m_HasFront;
	pBrushLayer->m_HasTele = R.m_HasTele;
	pBrushLayer->m_HasSwitch = R.m_HasSwitch;
	pBrushLayer->m_HasSpeedup = R.m_HasSpeedup;
	pBrushLayer->m_HasTune = R.m_HasTune;
	pBrushLayer->m_Image = R.m_ImageId;
	pBrushLayer->m_pTiles[0] = R.m_Tile;

	m_pBrush->Clear();
	m_pBrush->AddLayer(pBrushLayer);

	// Move this entry to the front of the recent list.
	if(Index != 0)
	{
		m_vRecentTiles.erase(m_vRecentTiles.begin() + Index);
		m_vRecentTiles.insert(m_vRecentTiles.begin(), R);
	}
}

float CEditor::FadeToolHash01(int x, int y, int Seed)
{
	uint32_t H = (uint32_t)x * 73856093U;
	H ^= (uint32_t)y * 19349663U;
	H ^= (uint32_t)Seed * 83492791U;
	H *= 0x9E3779B1U;
	H ^= H >> 16;
	return (float)(H & 0xFFFFFFU) / (float)0x1000000U;
}

vec2 CEditor::FadeToolBezierAt(vec2 P0, vec2 P1, vec2 P2, float t)
{
	const float u = 1.0f - t;
	return P0 * (u * u) + P1 * (2.0f * u * t) + P2 * (t * t);
}

vec2 CEditor::FadeToolCubicBezierAt(vec2 P0, vec2 H0, vec2 H1, vec2 P1, float t)
{
	const float u = 1.0f - t;
	const float u2 = u * u;
	const float u3 = u2 * u;
	const float t2 = t * t;
	const float t3 = t2 * t;
	return P0 * u3 + H0 * (3.0f * u2 * t) + H1 * (3.0f * u * t2) + P1 * t3;
}

void CEditor::FadeRecomputeAutoHandles(int NodeIndex)
{
	const int N = (int)m_vFadeNodes.size();
	if(NodeIndex < 0 || NodeIndex >= N)
		return;
	if((int)m_vFadeHandleIn.size() != N)
		m_vFadeHandleIn.resize(N, vec2(0.0f, 0.0f));
	if((int)m_vFadeHandleOut.size() != N)
		m_vFadeHandleOut.resize(N, vec2(0.0f, 0.0f));

	const vec2 P = m_vFadeNodes[NodeIndex];
	const vec2 Prev = (NodeIndex > 0) ? m_vFadeNodes[NodeIndex - 1] : (P + (P - m_vFadeNodes[std::min(N - 1, NodeIndex + 1)]));
	const vec2 Next = (NodeIndex + 1 < N) ? m_vFadeNodes[NodeIndex + 1] : (P + (P - m_vFadeNodes[std::max(0, NodeIndex - 1)]));
	// Catmull-Rom equivalent tangent — gives a smooth default identical to the old curve.
	const vec2 Tangent = (Next - Prev) * 0.25f;
	m_vFadeHandleIn[NodeIndex] = -Tangent;
	m_vFadeHandleOut[NodeIndex] = Tangent;
}

void CEditor::FadeToolGetClosest(vec2 P, float &OutT, float &OutDist) const
{
	const int N = (int)m_vFadeNodes.size();
	if(N < 2)
	{
		OutT = 0.0f;
		OutDist = 1e9f;
		return;
	}
	const bool HaveHandles = (int)m_vFadeHandleIn.size() == N && (int)m_vFadeHandleOut.size() == N;
	const int Segments = N - 1;
	const int SamplesPerSeg = 50;
	float BestDistSq = 1e18f;
	float BestGlobalT = 0.0f;
	for(int s = 0; s < Segments; ++s)
	{
		const vec2 &Pa = m_vFadeNodes[s];
		const vec2 &Pb = m_vFadeNodes[s + 1];
		const vec2 H0 = HaveHandles ? (Pa + m_vFadeHandleOut[s]) : Pa;
		const vec2 H1 = HaveHandles ? (Pb + m_vFadeHandleIn[s + 1]) : Pb;
		for(int i = 0; i <= SamplesPerSeg; ++i)
		{
			const float t = (float)i / (float)SamplesPerSeg;
			const vec2 Pt = FadeToolCubicBezierAt(Pa, H0, H1, Pb, t);
			const float Dx = P.x - Pt.x;
			const float Dy = P.y - Pt.y;
			const float DSq = Dx * Dx + Dy * Dy;
			if(DSq < BestDistSq)
			{
				BestDistSq = DSq;
				BestGlobalT = ((float)s + t) / (float)Segments;
			}
		}
	}
	OutT = BestGlobalT;
	OutDist = std::sqrt(BestDistSq);
}

int CEditor::FadeToolFindHoveredSegment(float wx, float wy, vec2 &OutClosest) const
{
	const int N = (int)m_vFadeNodes.size();
	if(N < 2)
		return -1;
	// Hit radius scales with the path's max width so clicking on the rasterized cells inserts a node.
	const int MaxW = std::max(m_FadeWidth, m_FadeWidthEnd);
	const float HoverRadius = std::max(20.0f, (float)MaxW * 32.0f + 16.0f);
	const bool HaveHandles = (int)m_vFadeHandleIn.size() == N && (int)m_vFadeHandleOut.size() == N;
	float BestDistSq = HoverRadius * HoverRadius;
	int BestSeg = -1;
	for(int s = 0; s < N - 1; ++s)
	{
		const vec2 &Pa = m_vFadeNodes[s];
		const vec2 &Pb = m_vFadeNodes[s + 1];
		const vec2 H0 = HaveHandles ? (Pa + m_vFadeHandleOut[s]) : Pa;
		const vec2 H1 = HaveHandles ? (Pb + m_vFadeHandleIn[s + 1]) : Pb;
		const int Samples = std::max(8, (int)(distance(Pa, Pb) / 4.0f));
		for(int i = 0; i <= Samples; ++i)
		{
			const float t = (float)i / (float)Samples;
			const vec2 Pt = FadeToolCubicBezierAt(Pa, H0, H1, Pb, t);
			const float Dx = wx - Pt.x;
			const float Dy = wy - Pt.y;
			const float DSq = Dx * Dx + Dy * Dy;
			if(DSq < BestDistSq)
			{
				BestDistSq = DSq;
				BestSeg = s;
				OutClosest = Pt;
			}
		}
	}
	return BestSeg;
}

float CEditor::FadeToolDensityAt(int CellX, int CellY) const
{
	const vec2 P(CellX * 32.0f + 16.0f, CellY * 32.0f + 16.0f);
	float t, Dist;
	FadeToolGetClosest(P, t, Dist);
	// Width varies along the path: lerp(start, end, t)
	const float WidthTilesAtT = (float)m_FadeWidth + ((float)m_FadeWidthEnd - (float)m_FadeWidth) * t;
	const float WidthW = WidthTilesAtT * 32.0f;
	const float FeatherW = (float)m_FadeFeather * 32.0f;
	if(Dist > WidthW + FeatherW)
		return 0.0f;
	const float Along = m_FadeDensityStart + (m_FadeDensityEnd - m_FadeDensityStart) * t;
	if(Dist <= WidthW || FeatherW <= 0.0f)
		return Along;
	const float Falloff = 1.0f - (Dist - WidthW) / FeatherW;
	return Along * Falloff;
}

bool CEditor::FadeToolIsCellFilled(int CellX, int CellY) const
{
	const float Density = FadeToolDensityAt(CellX, CellY);
	if(Density <= 0.0f)
		return false;
	if(Density >= 1.0f)
		return true;
	return FadeToolHash01(CellX, CellY, m_FadeSeed) < Density;
}

void CEditor::FadeToolEnsureInit()
{
	if(m_FadeInit)
		return;
	const vec2 Center = MapView()->GetWorldOffset();
	const float HalfLen = 16.0f * 32.0f;
	m_vFadeNodes.clear();
	m_vFadeNodes.push_back(vec2(Center.x - HalfLen, Center.y));
	m_vFadeNodes.push_back(vec2(Center.x, Center.y));
	m_vFadeNodes.push_back(vec2(Center.x + HalfLen, Center.y));
	m_vFadeHandleIn.assign(m_vFadeNodes.size(), vec2(0.0f, 0.0f));
	m_vFadeHandleOut.assign(m_vFadeNodes.size(), vec2(0.0f, 0.0f));
	for(int i = 0; i < (int)m_vFadeNodes.size(); ++i)
		FadeRecomputeAutoHandles(i);
	m_FadeInit = true;
}

void CEditor::FadeToolCancel()
{
	m_FadeInit = false;
	m_FadeActiveHandle = -1;
	m_FadeDragKind = FADE_DRAG_NONE;
	m_vFadeNodes.clear();
	m_vFadeHandleIn.clear();
	m_vFadeHandleOut.clear();
}

int CEditor::FadeToolHandleHitTest(float wx, float wy, int &OutKind) const
{
	OutKind = FADE_DRAG_NONE;
	const int N = (int)m_vFadeNodes.size();
	if(N == 0 || (int)m_vFadeHandleIn.size() != N || (int)m_vFadeHandleOut.size() != N)
		return -1;
	const float HitRange = 16.0f;
	int Best = -1;
	int BestKind = FADE_DRAG_NONE;
	float BestDistSq = HitRange * HitRange;
	for(int i = 0; i < N; ++i)
	{
		// Don't show "in" handle for the very first node, "out" handle for the last — they
		// don't influence any segment, so they're hidden from interaction too.
		if(i > 0)
		{
			const vec2 H = m_vFadeNodes[i] + m_vFadeHandleIn[i];
			const float Dx = wx - H.x, Dy = wy - H.y;
			const float DSq = Dx * Dx + Dy * Dy;
			if(DSq < BestDistSq)
			{
				BestDistSq = DSq;
				Best = i;
				BestKind = FADE_DRAG_HANDLE_IN;
			}
		}
		if(i < N - 1)
		{
			const vec2 H = m_vFadeNodes[i] + m_vFadeHandleOut[i];
			const float Dx = wx - H.x, Dy = wy - H.y;
			const float DSq = Dx * Dx + Dy * Dy;
			if(DSq < BestDistSq)
			{
				BestDistSq = DSq;
				Best = i;
				BestKind = FADE_DRAG_HANDLE_OUT;
			}
		}
	}
	OutKind = BestKind;
	return Best;
}

int CEditor::FadeToolHitTest(float wx, float wy) const
{
	if(!m_FadeInit || m_vFadeNodes.size() < 2)
		return -1;
	const float HitRange = 26.0f;
	int Best = -1;
	float BestDistSq = HitRange * HitRange;
	for(int i = 0; i < (int)m_vFadeNodes.size(); ++i)
	{
		const float Dx = wx - m_vFadeNodes[i].x;
		const float Dy = wy - m_vFadeNodes[i].y;
		const float DSq = Dx * Dx + Dy * Dy;
		if(DSq < BestDistSq)
		{
			BestDistSq = DSq;
			Best = i;
		}
	}
	return Best;
}

void CEditor::FadeToolApplyDrag(float wx, float wy)
{
	const int N = (int)m_vFadeNodes.size();
	if(m_FadeActiveHandle < 0 || m_FadeActiveHandle >= N)
		return;
	const float Dx = wx - m_FadeDragStartMouse.x;
	const float Dy = wy - m_FadeDragStartMouse.y;

	if(m_FadeDragKind == FADE_DRAG_HANDLE_IN)
	{
		if((int)m_vFadeHandleIn.size() == N)
			m_vFadeHandleIn[m_FadeActiveHandle] = m_FadeDragStartHandleIn + vec2(Dx, Dy);
	}
	else if(m_FadeDragKind == FADE_DRAG_HANDLE_OUT)
	{
		if((int)m_vFadeHandleOut.size() == N)
			m_vFadeHandleOut[m_FadeActiveHandle] = m_FadeDragStartHandleOut + vec2(Dx, Dy);
	}
	else
	{
		m_vFadeNodes[m_FadeActiveHandle] = m_FadeDragStartNodePos + vec2(Dx, Dy);
		// Refresh auto-handles around moved node so the curve stays smooth — but only on
		// nodes whose handles are still at their default (we don't know that without a flag,
		// so for simplicity we only auto-update *adjacent* nodes' implicit handles, leaving
		// the moved node's own handles untouched so any explicit user shaping survives).
		const int Idx = m_FadeActiveHandle;
		if(Idx > 0)
			FadeRecomputeAutoHandles(Idx - 1);
		if(Idx + 1 < N)
			FadeRecomputeAutoHandles(Idx + 1);
	}
}

void CEditor::FadeToolRender(float wx, float wy)
{
	if(!m_FadeInit || !m_FadeTool || m_vFadeNodes.size() < 2)
		return;
	if(m_FadeActiveHandle >= 0)
	{
		m_FadeHoverHandle = -1;
		m_FadeHoverDragKind = FADE_DRAG_NONE;
	}
	else
	{
		// Handles take priority over the node body so the user can grab the dot even when the
		// node hit area would otherwise eat the click.
		int HoverKind = FADE_DRAG_NONE;
		int Hover = FadeToolHandleHitTest(wx, wy, HoverKind);
		if(Hover < 0)
		{
			Hover = FadeToolHitTest(wx, wy);
			HoverKind = (Hover >= 0) ? (int)FADE_DRAG_NODE : (int)FADE_DRAG_NONE;
		}
		m_FadeHoverHandle = Hover;
		m_FadeHoverDragKind = HoverKind;
	}

	// Bounding box of the path + width + feather
	float MinX = 1e9f, MaxX = -1e9f, MinY = 1e9f, MaxY = -1e9f;
	for(const vec2 &N : m_vFadeNodes)
	{
		if(N.x < MinX) MinX = N.x;
		if(N.x > MaxX) MaxX = N.x;
		if(N.y < MinY) MinY = N.y;
		if(N.y > MaxY) MaxY = N.y;
	}
	const float Margin = (float)(std::max(m_FadeWidth, m_FadeWidthEnd) + m_FadeFeather) * 32.0f;
	MinX -= Margin; MaxX += Margin;
	MinY -= Margin; MaxY += Margin;
	const int X1 = (int)std::floor(MinX / 32.0f);
	const int Y1 = (int)std::floor(MinY / 32.0f);
	const int X2 = (int)std::floor(MaxX / 32.0f);
	const int Y2 = (int)std::floor(MaxY / 32.0f);

	// Fill cells preview
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 0.9f, 0.4f, 0.5f);
	for(int y = Y1; y <= Y2; ++y)
	{
		for(int x = X1; x <= X2; ++x)
		{
			if(FadeToolIsCellFilled(x, y))
			{
				IGraphics::CQuadItem Q(x * 32.0f, y * 32.0f, 32.0f, 32.0f);
				Graphics()->QuadsDrawTL(&Q, 1);
			}
		}
	}
	Graphics()->QuadsEnd();

	const int N = (int)m_vFadeNodes.size();
	const bool HaveHandles = (int)m_vFadeHandleIn.size() == N && (int)m_vFadeHandleOut.size() == N;

	// Curve overlay (cubic bezier per segment using node + per-side handles)
	{
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(0.4f, 0.8f, 1.0f, 1.0f);
		for(int s = 0; s < N - 1; ++s)
		{
			const vec2 &Pa = m_vFadeNodes[s];
			const vec2 &Pb = m_vFadeNodes[s + 1];
			const vec2 H0 = HaveHandles ? (Pa + m_vFadeHandleOut[s]) : Pa;
			const vec2 H1 = HaveHandles ? (Pb + m_vFadeHandleIn[s + 1]) : Pb;
			const int CurveSamples = std::max(16, (int)(distance(Pa, Pb) / 4.0f));
			vec2 Prev = Pa;
			for(int i = 1; i <= CurveSamples; ++i)
			{
				const float t = (float)i / (float)CurveSamples;
				const vec2 Cur = FadeToolCubicBezierAt(Pa, H0, H1, Pb, t);
				IGraphics::CLineItem L(Prev.x, Prev.y, Cur.x, Cur.y);
				Graphics()->LinesDraw(&L, 1);
				Prev = Cur;
			}
		}
		Graphics()->LinesEnd();
	}

	// Handle stems (thin lines from node to handle dot)
	if(HaveHandles)
	{
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->SetColor(0.6f, 0.7f, 0.85f, 0.7f);
		for(int i = 0; i < N; ++i)
		{
			const vec2 &P = m_vFadeNodes[i];
			if(i > 0)
			{
				const vec2 H = P + m_vFadeHandleIn[i];
				IGraphics::CLineItem L(P.x, P.y, H.x, H.y);
				Graphics()->LinesDraw(&L, 1);
			}
			if(i < N - 1)
			{
				const vec2 H = P + m_vFadeHandleOut[i];
				IGraphics::CLineItem L(P.x, P.y, H.x, H.y);
				Graphics()->LinesDraw(&L, 1);
			}
		}
		Graphics()->LinesEnd();
	}

	// Handle dots — smaller, lighter than nodes; orange when hovered/active
	if(HaveHandles)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		for(int i = 0; i < N; ++i)
		{
			const vec2 &P = m_vFadeNodes[i];
			if(i > 0)
			{
				const bool Active = (m_FadeActiveHandle == i && m_FadeDragKind == FADE_DRAG_HANDLE_IN) || (m_FadeHoverHandle == i && m_FadeHoverDragKind == FADE_DRAG_HANDLE_IN);
				if(Active)
					Graphics()->SetColor(1.0f, 0.55f, 0.1f, 1.0f);
				else
					Graphics()->SetColor(0.85f, 0.92f, 1.0f, 0.9f);
				const vec2 H = P + m_vFadeHandleIn[i];
				DrawFilledCircle(Graphics(), H.x, H.y, Active ? 8.0f : 6.0f);
			}
			if(i < N - 1)
			{
				const bool Active = (m_FadeActiveHandle == i && m_FadeDragKind == FADE_DRAG_HANDLE_OUT) || (m_FadeHoverHandle == i && m_FadeHoverDragKind == FADE_DRAG_HANDLE_OUT);
				if(Active)
					Graphics()->SetColor(1.0f, 0.55f, 0.1f, 1.0f);
				else
					Graphics()->SetColor(0.85f, 0.92f, 1.0f, 0.9f);
				const vec2 H = P + m_vFadeHandleOut[i];
				DrawFilledCircle(Graphics(), H.x, H.y, Active ? 8.0f : 6.0f);
			}
		}
		Graphics()->QuadsEnd();
	}

	// Nodes (circular, larger than handles)
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	for(int i = 0; i < N; ++i)
	{
		const bool NodeActive = (m_FadeActiveHandle == i && m_FadeDragKind == FADE_DRAG_NODE);
		const bool NodeHover = (m_FadeHoverHandle == i && m_FadeHoverDragKind == FADE_DRAG_NODE);
		const bool Highlighted = NodeActive || NodeHover;
		const float Radius = Highlighted ? 14.0f : 10.0f;
		if(Highlighted)
			Graphics()->SetColor(1.0f, 0.5f, 0.0f, 1.0f);
		else
			Graphics()->SetColor(0.4f, 0.8f, 1.0f, 1.0f);
		const vec2 &P = m_vFadeNodes[i];
		DrawFilledCircle(Graphics(), P.x, P.y, Radius);
	}
	Graphics()->QuadsEnd();
}

void CEditor::FadeToolCommit()
{
	if(!m_FadeInit || m_vFadeNodes.size() < 2 || m_pBrush->IsEmpty())
	{
		FadeToolCancel();
		return;
	}

	std::shared_ptr<CLayer> pBrushLayer;
	for(const auto &pLayer : m_pBrush->m_vpLayers)
	{
		if(pLayer->m_Type != LAYERTYPE_TILES)
			continue;
		auto pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
		if(pTiles->m_Width != 1 || pTiles->m_Height != 1)
			continue;
		pBrushLayer = pLayer;
		break;
	}
	if(!pBrushLayer)
	{
		FadeToolCancel();
		return;
	}

	float MinX = 1e9f, MaxX = -1e9f, MinY = 1e9f, MaxY = -1e9f;
	for(const vec2 &N : m_vFadeNodes)
	{
		if(N.x < MinX) MinX = N.x;
		if(N.x > MaxX) MaxX = N.x;
		if(N.y < MinY) MinY = N.y;
		if(N.y > MaxY) MaxY = N.y;
	}
	const float Margin = (float)(std::max(m_FadeWidth, m_FadeWidthEnd) + m_FadeFeather) * 32.0f;
	MinX -= Margin; MaxX += Margin;
	MinY -= Margin; MaxY += Margin;
	const int X1 = (int)std::floor(MinX / 32.0f);
	const int Y1 = (int)std::floor(MinY / 32.0f);
	const int X2 = (int)std::floor(MaxX / 32.0f);
	const int Y2 = (int)std::floor(MaxY / 32.0f);

	const bool SavedDestructive = m_BrushDrawDestructive;
	m_BrushDrawDestructive = true;

	for(int LayerIdx : Map()->m_vSelectedLayers)
	{
		if(LayerIdx < 0 || LayerIdx >= (int)Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers.size())
			continue;
		std::shared_ptr<CLayer> pLayer = Map()->m_vpGroups[Map()->m_SelectedGroup]->m_vpLayers[LayerIdx];
		if(pLayer->m_Type != LAYERTYPE_TILES)
			continue;
		auto pTiles = std::static_pointer_cast<CLayerTiles>(pLayer);
		auto pBrushTiles = std::static_pointer_cast<CLayerTiles>(pBrushLayer);
		if(!((!pTiles->m_HasTele || pBrushTiles->m_HasTele) && (!pTiles->m_HasSpeedup || pBrushTiles->m_HasSpeedup) && (!pTiles->m_HasFront || pBrushTiles->m_HasFront) && (!pTiles->m_HasGame || pBrushTiles->m_HasGame) && (!pTiles->m_HasSwitch || pBrushTiles->m_HasSwitch) && (!pTiles->m_HasTune || pBrushTiles->m_HasTune)))
			continue;

		for(int y = Y1; y <= Y2; ++y)
		{
			for(int x = X1; x <= X2; ++x)
			{
				if(x < 0 || x >= pTiles->m_Width || y < 0 || y >= pTiles->m_Height)
					continue;
				if(FadeToolIsCellFilled(x, y))
					pTiles->BrushDraw(pBrushLayer.get(), vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f));
				else if(m_FadeAirOnUnfilled)
					pTiles->SetTile(x, y, CTile{TILE_AIR});
			}
		}
	}

	m_BrushDrawDestructive = SavedDestructive;

	std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
	if(!pAction->IsEmpty())
		Map()->m_EditorHistory.RecordAction(pAction);
}

IEditor *CreateEditor() { return new CEditor; }
