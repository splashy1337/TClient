/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "layer_tiles.h"

#include "image.h"
#include "layer_speedup.h"
#include "layer_switch.h"
#include "layer_tele.h"
#include "layer_tune.h"

#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/map.h>

#include <game/editor/editor.h>
#include <game/editor/editor_actions.h>
#include <game/editor/enums.h>

#include <base/math.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
#include <utility>
#include <vector>

CLayerTiles::CLayerTiles(CEditorMap *pMap, int w, int h) :
	CLayer(pMap, LAYERTYPE_TILES)
{
	m_aName[0] = '\0';
	m_Width = w;
	m_Height = h;
	m_Image = -1;
	m_HasGame = false;
	m_Color.r = 255;
	m_Color.g = 255;
	m_Color.b = 255;
	m_Color.a = 255;
	m_ColorEnv = -1;
	m_ColorEnvOffset = 0;

	m_HasTele = false;
	m_HasSpeedup = false;
	m_HasFront = false;
	m_HasSwitch = false;
	m_HasTune = false;
	m_AutoMapperConfig = -1;
	m_AutoMapperReference = -1;
	m_Seed = 0;
	m_AutoAutoMap = false;

	m_pTiles = new CTile[m_Width * m_Height];
	mem_zero(m_pTiles, (size_t)m_Width * m_Height * sizeof(CTile));
}

CLayerTiles::CLayerTiles(const CLayerTiles &Other) :
	CLayer(Other)
{
	m_Width = Other.m_Width;
	m_Height = Other.m_Height;
	m_pTiles = new CTile[m_Width * m_Height];
	mem_copy(m_pTiles, Other.m_pTiles, (size_t)m_Width * m_Height * sizeof(CTile));

	m_Image = Other.m_Image;
	m_HasGame = Other.m_HasGame;
	m_Color = Other.m_Color;
	m_ColorEnv = Other.m_ColorEnv;
	m_ColorEnvOffset = Other.m_ColorEnvOffset;

	m_AutoMapperConfig = Other.m_AutoMapperConfig;
	m_AutoMapperReference = Other.m_AutoMapperReference;
	m_Seed = Other.m_Seed;
	m_AutoAutoMap = Other.m_AutoAutoMap;
	m_HasTele = Other.m_HasTele;
	m_HasSpeedup = Other.m_HasSpeedup;
	m_HasFront = Other.m_HasFront;
	m_HasSwitch = Other.m_HasSwitch;
	m_HasTune = Other.m_HasTune;

	str_copy(m_aFilename, Other.m_aFilename);
}

CLayerTiles::~CLayerTiles()
{
	delete[] m_pTiles;
}

CTile CLayerTiles::GetTile(int x, int y) const
{
	return m_pTiles[y * m_Width + x];
}

void CLayerTiles::SetTile(int x, int y, CTile Tile)
{
	auto CurrentTile = m_pTiles[y * m_Width + x];
	SetTileIgnoreHistory(x, y, Tile);
	RecordStateChange(x, y, CurrentTile, Tile);

	if(m_FillGameTile != -1 && m_LiveGameTiles)
	{
		std::shared_ptr<CLayerTiles> pLayer = Map()->m_pGameLayer;
		if(m_FillGameTile == TILE_TELECHECKIN || m_FillGameTile == TILE_TELECHECKINEVIL)
		{
			if(!Map()->m_pTeleLayer)
			{
				std::shared_ptr<CLayerTele> pLayerTele = std::make_shared<CLayerTele>(Map(), m_Width, m_Height);
				Map()->MakeTeleLayer(pLayerTele);
				Map()->m_pGameGroup->AddLayer(pLayerTele);
				int GameGroupIndex = std::find(Map()->m_vpGroups.begin(), Map()->m_vpGroups.end(), Map()->m_pGameGroup) - Map()->m_vpGroups.begin();
				int LayerIndex = Map()->m_vpGroups[GameGroupIndex]->m_vpLayers.size() - 1;
				Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionAddLayer>(Map(), GameGroupIndex, LayerIndex));
			}

			pLayer = Map()->m_pTeleLayer;
		}

		bool HasTile = Tile.m_Index != 0;
		pLayer->SetTile(x, y, CTile{(unsigned char)(HasTile ? m_FillGameTile : TILE_AIR)});
	}

	// Symmetry mirror: re-emit at mirrored cell positions. Static guard prevents infinite recursion.
	static thread_local bool s_InMirror = false;
	if(!s_InMirror)
	{
		const int Mode = Editor()->m_SymmetryMode;
		if(Mode != 0)
		{
			const vec2 C = Editor()->m_SymmetryCenter;
			const float WorldX = x * 32.0f + 16.0f;
			const float WorldY = y * 32.0f + 16.0f;
			const bool MirrorH = (Mode == 1 || Mode == 3);
			const bool MirrorV = (Mode == 2 || Mode == 3);
			const int Mx = (int)std::floor((2.0f * C.x - WorldX) / 32.0f);
			const int My = (int)std::floor((2.0f * C.y - WorldY) / 32.0f);
			const bool IsEntity = m_HasGame || m_HasFront || m_HasTele || m_HasSwitch || m_HasSpeedup || m_HasTune;
			s_InMirror = true;
			if(MirrorH && Mx != x && Mx >= 0 && Mx < m_Width)
			{
				CTile MTile = Tile;
				if(!IsEntity)
					MTile.m_Flags ^= TILEFLAG_XFLIP;
				SetTile(Mx, y, MTile);
			}
			if(MirrorV && My != y && My >= 0 && My < m_Height)
			{
				CTile MTile = Tile;
				if(!IsEntity)
					MTile.m_Flags ^= TILEFLAG_YFLIP;
				SetTile(x, My, MTile);
			}
			if(MirrorH && MirrorV && (Mx != x || My != y) && Mx >= 0 && Mx < m_Width && My >= 0 && My < m_Height)
			{
				CTile MTile = Tile;
				if(!IsEntity)
					MTile.m_Flags ^= TILEFLAG_XFLIP | TILEFLAG_YFLIP;
				SetTile(Mx, My, MTile);
			}
			s_InMirror = false;
		}
	}
}

void CLayerTiles::SetTileIgnoreHistory(int x, int y, CTile Tile) const
{
	m_pTiles[y * m_Width + x] = Tile;
}

void CLayerTiles::RecordStateChange(int x, int y, CTile Previous, CTile Tile)
{
	if(!m_TilesHistory[y][x].m_Changed)
		m_TilesHistory[y][x] = STileStateChange{true, Previous, Tile};
	else
		m_TilesHistory[y][x].m_Current = Tile;
}

void CLayerTiles::PrepareForSave()
{
	for(int y = 0; y < m_Height; y++)
		for(int x = 0; x < m_Width; x++)
			m_pTiles[y * m_Width + x].m_Flags &= TILEFLAG_XFLIP | TILEFLAG_YFLIP | TILEFLAG_ROTATE;

	if(m_Image != -1 && m_Color.a == 255)
	{
		for(int y = 0; y < m_Height; y++)
			for(int x = 0; x < m_Width; x++)
				m_pTiles[y * m_Width + x].m_Flags |= Map()->m_vpImages[m_Image]->m_aTileFlags[m_pTiles[y * m_Width + x].m_Index];
	}
}

void CLayerTiles::ExtractTiles(const CTile *pSavedTiles, size_t SavedTilesSize) const
{
	const size_t DestSize = (size_t)m_Width * m_Height;
	if(SavedTilesSize >= DestSize)
	{
		mem_copy(m_pTiles, pSavedTiles, DestSize * sizeof(CTile));
		for(size_t TileIndex = 0; TileIndex < DestSize; ++TileIndex)
		{
			m_pTiles[TileIndex].m_Skip = 0;
			m_pTiles[TileIndex].m_Reserved = 0;
		}
	}
}

void CLayerTiles::MakePalette() const
{
	for(int y = 0; y < m_Height; y++)
		for(int x = 0; x < m_Width; x++)
			m_pTiles[y * m_Width + x].m_Index = y * 16 + x;
}

void CLayerTiles::Render(bool Tileset)
{
	IGraphics::CTextureHandle Texture;
	if(m_Image >= 0 && (size_t)m_Image < Map()->m_vpImages.size())
		Texture = Map()->m_vpImages[m_Image]->m_Texture;
	else if(m_HasGame)
		Texture = Editor()->GetEntitiesTexture();
	else if(m_HasFront)
		Texture = Editor()->GetFrontTexture();
	else if(m_HasTele)
		Texture = Editor()->GetTeleTexture();
	else if(m_HasSpeedup)
		Texture = Editor()->GetSpeedupTexture();
	else if(m_HasSwitch)
		Texture = Editor()->GetSwitchTexture();
	else if(m_HasTune)
		Texture = Editor()->GetTuneTexture();
	Graphics()->TextureSet(Texture);

	ColorRGBA ColorEnv = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	Editor()->EnvelopeEval(m_ColorEnvOffset, m_ColorEnv, ColorEnv, 4);
	const ColorRGBA Color = ColorRGBA(m_Color.r / 255.0f, m_Color.g / 255.0f, m_Color.b / 255.0f, m_Color.a / 255.0f).Multiply(ColorEnv);

	Graphics()->BlendNone();
	Editor()->RenderMap()->RenderTilemap(m_pTiles, m_Width, m_Height, 32.0f, Color, LAYERRENDERFLAG_OPAQUE);
	Graphics()->BlendNormal();
	Editor()->RenderMap()->RenderTilemap(m_pTiles, m_Width, m_Height, 32.0f, Color, LAYERRENDERFLAG_TRANSPARENT);

	// Render DDRace Layers
	if(!Tileset)
	{
		int OverlayRenderFlags = (g_Config.m_ClTextEntitiesEditor ? OVERLAYRENDERFLAG_TEXT : 0) | OVERLAYRENDERFLAG_EDITOR;
		if(m_HasTele)
			Editor()->RenderMap()->RenderTeleOverlay(static_cast<CLayerTele *>(this)->m_pTeleTile, m_Width, m_Height, 32.0f, OverlayRenderFlags);
		if(m_HasSpeedup)
			Editor()->RenderMap()->RenderSpeedupOverlay(static_cast<CLayerSpeedup *>(this)->m_pSpeedupTile, m_Width, m_Height, 32.0f, OverlayRenderFlags);
		if(m_HasSwitch)
			Editor()->RenderMap()->RenderSwitchOverlay(static_cast<CLayerSwitch *>(this)->m_pSwitchTile, m_Width, m_Height, 32.0f, OverlayRenderFlags);
		if(m_HasTune)
			Editor()->RenderMap()->RenderTuneOverlay(static_cast<CLayerTune *>(this)->m_pTuneTile, m_Width, m_Height, 32.0f, OverlayRenderFlags);
	}
}

int CLayerTiles::ConvertX(float x) const { return (int)(x / 32.0f); }
int CLayerTiles::ConvertY(float y) const { return (int)(y / 32.0f); }

void CLayerTiles::Convert(CUIRect Rect, CIntRect *pOut) const
{
	pOut->x = ConvertX(Rect.x);
	pOut->y = ConvertY(Rect.y);
	pOut->w = ConvertX(Rect.x + Rect.w + 31) - pOut->x;
	pOut->h = ConvertY(Rect.y + Rect.h + 31) - pOut->y;
}

void CLayerTiles::Snap(CUIRect *pRect) const
{
	CIntRect Out;
	Convert(*pRect, &Out);
	pRect->x = Out.x * 32.0f;
	pRect->y = Out.y * 32.0f;
	pRect->w = Out.w * 32.0f;
	pRect->h = Out.h * 32.0f;
}

void CLayerTiles::Clamp(CIntRect *pRect) const
{
	if(pRect->x < 0)
	{
		pRect->w += pRect->x;
		pRect->x = 0;
	}

	if(pRect->y < 0)
	{
		pRect->h += pRect->y;
		pRect->y = 0;
	}

	if(pRect->x + pRect->w > m_Width)
		pRect->w = m_Width - pRect->x;

	if(pRect->y + pRect->h > m_Height)
		pRect->h = m_Height - pRect->y;

	if(pRect->h < 0)
		pRect->h = 0;
	if(pRect->w < 0)
		pRect->w = 0;
}

bool CLayerTiles::IsEntitiesLayer() const
{
	return Map()->m_pGameLayer.get() == this || Map()->m_pTeleLayer.get() == this || Map()->m_pSpeedupLayer.get() == this || Map()->m_pFrontLayer.get() == this || Map()->m_pSwitchLayer.get() == this || Map()->m_pTuneLayer.get() == this;
}

bool CLayerTiles::IsEmpty() const
{
	for(int y = 0; y < m_Height; y++)
	{
		for(int x = 0; x < m_Width; x++)
		{
			if(GetTile(x, y).m_Index != 0)
			{
				return false;
			}
		}
	}
	return true;
}

void CLayerTiles::BrushSelecting(CUIRect Rect)
{
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
	Snap(&Rect);
	IGraphics::CQuadItem QuadItem(Rect.x, Rect.y, Rect.w, Rect.h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	char aBuf[16];
	str_format(aBuf, sizeof(aBuf), "%d⨯%d", ConvertX(Rect.w), ConvertY(Rect.h));
	TextRender()->Text(Rect.x + 3.0f, Rect.y + 3.0f, Editor()->m_ShowPicker ? 15.0f : Editor()->MapView()->ScaleLength(15.0f), aBuf, -1.0f);
}

template<typename T>
static void InitGrabbedLayer(std::shared_ptr<T> &pLayer, CLayerTiles *pThisLayer)
{
	pLayer->m_Image = pThisLayer->m_Image;
	pLayer->m_HasGame = pThisLayer->m_HasGame;
	pLayer->m_HasFront = pThisLayer->m_HasFront;
	pLayer->m_HasTele = pThisLayer->m_HasTele;
	pLayer->m_HasSpeedup = pThisLayer->m_HasSpeedup;
	pLayer->m_HasSwitch = pThisLayer->m_HasSwitch;
	pLayer->m_HasTune = pThisLayer->m_HasTune;
	if(pThisLayer->Editor()->m_BrushColorEnabled)
	{
		pLayer->m_Color = pThisLayer->m_Color;
		pLayer->m_Color.a = 255;
	}
}

int CLayerTiles::BrushGrab(CLayerGroup *pBrush, CUIRect Rect)
{
	CIntRect r;
	Convert(Rect, &r);
	Clamp(&r);

	if(!r.w || !r.h)
		return 0;

	// create new layers
	if(m_HasTele)
	{
		std::shared_ptr<CLayerTele> pGrabbed = std::make_shared<CLayerTele>(pBrush->Map(), r.w, r.h);
		InitGrabbedLayer(pGrabbed, this);

		pBrush->AddLayer(pGrabbed);

		for(int y = 0; y < r.h; y++)
		{
			for(int x = 0; x < r.w; x++)
			{
				// copy the tiles
				pGrabbed->m_pTiles[y * pGrabbed->m_Width + x] = GetTile(r.x + x, r.y + y);

				// copy the tele data
				if(!Editor()->Input()->KeyIsPressed(KEY_SPACE))
				{
					pGrabbed->m_pTeleTile[y * pGrabbed->m_Width + x] = static_cast<CLayerTele *>(this)->m_pTeleTile[(r.y + y) * m_Width + (r.x + x)];
					unsigned char TgtIndex = pGrabbed->m_pTeleTile[y * pGrabbed->m_Width + x].m_Type;
					if(IsValidTeleTile(TgtIndex))
					{
						if(IsTeleTileNumberUsed(TgtIndex, false))
							Editor()->m_TeleNumber = pGrabbed->m_pTeleTile[y * pGrabbed->m_Width + x].m_Number;
						else if(IsTeleTileNumberUsed(TgtIndex, true))
							Editor()->m_TeleCheckpointNumber = pGrabbed->m_pTeleTile[y * pGrabbed->m_Width + x].m_Number;
					}
				}
				else
				{
					const CTile &Tile = pGrabbed->m_pTiles[y * pGrabbed->m_Width + x];
					if(IsValidTeleTile(Tile.m_Index) && IsTeleTileNumberUsedAny(Tile.m_Index))
					{
						pGrabbed->m_pTeleTile[y * pGrabbed->m_Width + x].m_Type = Tile.m_Index;
						pGrabbed->m_pTeleTile[y * pGrabbed->m_Width + x].m_Number = IsTeleTileCheckpoint(Tile.m_Index) ? Editor()->m_TeleCheckpointNumber : Editor()->m_TeleNumber;
					}
				}
			}
		}

		pGrabbed->m_TeleNumber = Editor()->m_TeleNumber;
		pGrabbed->m_TeleCheckpointNumber = Editor()->m_TeleCheckpointNumber;

		str_copy(pGrabbed->m_aFilename, pGrabbed->Map()->m_aFilename);
	}
	else if(m_HasSpeedup)
	{
		std::shared_ptr<CLayerSpeedup> pGrabbed = std::make_shared<CLayerSpeedup>(pBrush->Map(), r.w, r.h);
		InitGrabbedLayer(pGrabbed, this);

		pBrush->AddLayer(pGrabbed);

		for(int y = 0; y < r.h; y++)
		{
			for(int x = 0; x < r.w; x++)
			{
				// copy the tiles
				pGrabbed->m_pTiles[y * pGrabbed->m_Width + x] = GetTile(r.x + x, r.y + y);

				// copy the speedup data
				if(!Editor()->Input()->KeyIsPressed(KEY_SPACE))
				{
					pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x] = static_cast<CLayerSpeedup *>(this)->m_pSpeedupTile[(r.y + y) * m_Width + (r.x + x)];
					if(IsValidSpeedupTile(pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_Type))
					{
						Editor()->m_SpeedupAngle = pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_Angle;
						Editor()->m_SpeedupForce = pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_Force;
						Editor()->m_SpeedupMaxSpeed = pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_MaxSpeed;
					}
				}
				else
				{
					const CTile &Tile = pGrabbed->m_pTiles[y * pGrabbed->m_Width + x];
					if(IsValidSpeedupTile(Tile.m_Index))
					{
						pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_Type = Tile.m_Index;
						pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_Angle = Editor()->m_SpeedupAngle;
						pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_Force = Editor()->m_SpeedupForce;
						pGrabbed->m_pSpeedupTile[y * pGrabbed->m_Width + x].m_MaxSpeed = Editor()->m_SpeedupMaxSpeed;
					}
				}
			}
		}

		pGrabbed->m_SpeedupForce = Editor()->m_SpeedupForce;
		pGrabbed->m_SpeedupMaxSpeed = Editor()->m_SpeedupMaxSpeed;
		pGrabbed->m_SpeedupAngle = Editor()->m_SpeedupAngle;
		str_copy(pGrabbed->m_aFilename, pGrabbed->Map()->m_aFilename);
	}
	else if(m_HasSwitch)
	{
		std::shared_ptr<CLayerSwitch> pGrabbed = std::make_shared<CLayerSwitch>(pBrush->Map(), r.w, r.h);
		InitGrabbedLayer(pGrabbed, this);

		pBrush->AddLayer(pGrabbed);

		for(int y = 0; y < r.h; y++)
		{
			for(int x = 0; x < r.w; x++)
			{
				// copy the tiles
				pGrabbed->m_pTiles[y * pGrabbed->m_Width + x] = GetTile(r.x + x, r.y + y);

				// copy the switch data
				if(!Editor()->Input()->KeyIsPressed(KEY_SPACE))
				{
					pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x] = static_cast<CLayerSwitch *>(this)->m_pSwitchTile[(r.y + y) * m_Width + (r.x + x)];
					if(IsValidSwitchTile(pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x].m_Type))
					{
						Editor()->m_SwitchNumber = pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x].m_Number;
						Editor()->m_SwitchDelay = pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x].m_Delay;
					}
				}
				else
				{
					const CTile &Tile = pGrabbed->m_pTiles[y * pGrabbed->m_Width + x];
					if(IsValidSwitchTile(Tile.m_Index))
					{
						pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x].m_Type = Tile.m_Index;
						pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x].m_Number = Editor()->m_SwitchNumber;
						pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x].m_Delay = Editor()->m_SwitchDelay;
						pGrabbed->m_pSwitchTile[y * pGrabbed->m_Width + x].m_Flags = Tile.m_Flags;
					}
				}
			}
		}

		pGrabbed->m_SwitchNumber = Editor()->m_SwitchNumber;
		pGrabbed->m_SwitchDelay = Editor()->m_SwitchDelay;
		str_copy(pGrabbed->m_aFilename, pGrabbed->Map()->m_aFilename);
	}

	else if(m_HasTune)
	{
		std::shared_ptr<CLayerTune> pGrabbed = std::make_shared<CLayerTune>(pBrush->Map(), r.w, r.h);
		InitGrabbedLayer(pGrabbed, this);

		pBrush->AddLayer(pGrabbed);

		// copy the tiles
		for(int y = 0; y < r.h; y++)
		{
			for(int x = 0; x < r.w; x++)
			{
				pGrabbed->m_pTiles[y * pGrabbed->m_Width + x] = GetTile(r.x + x, r.y + y);

				if(!Editor()->Input()->KeyIsPressed(KEY_SPACE))
				{
					pGrabbed->m_pTuneTile[y * pGrabbed->m_Width + x] = static_cast<CLayerTune *>(this)->m_pTuneTile[(r.y + y) * m_Width + (r.x + x)];
					if(IsValidTuneTile(pGrabbed->m_pTuneTile[y * pGrabbed->m_Width + x].m_Type))
					{
						Editor()->m_TuningNumber = pGrabbed->m_pTuneTile[y * pGrabbed->m_Width + x].m_Number;
					}
				}
				else
				{
					const CTile &Tile = pGrabbed->m_pTiles[y * pGrabbed->m_Width + x];
					if(IsValidTuneTile(Tile.m_Index))
					{
						pGrabbed->m_pTuneTile[y * pGrabbed->m_Width + x].m_Type = Tile.m_Index;
						pGrabbed->m_pTuneTile[y * pGrabbed->m_Width + x].m_Number = Editor()->m_TuningNumber;
					}
				}
			}
		}

		pGrabbed->m_TuningNumber = Editor()->m_TuningNumber;
		str_copy(pGrabbed->m_aFilename, pGrabbed->Map()->m_aFilename);
	}
	else // game, front and tiles layers
	{
		std::shared_ptr<CLayerTiles> pGrabbed;
		if(m_HasGame)
		{
			pGrabbed = std::make_shared<CLayerGame>(pBrush->Map(), r.w, r.h);
		}
		else if(m_HasFront)
		{
			pGrabbed = std::make_shared<CLayerFront>(pBrush->Map(), r.w, r.h);
		}
		else
		{
			pGrabbed = std::make_shared<CLayerTiles>(pBrush->Map(), r.w, r.h);
		}
		InitGrabbedLayer(pGrabbed, this);

		pBrush->AddLayer(pGrabbed);

		// copy the tiles
		for(int y = 0; y < r.h; y++)
			for(int x = 0; x < r.w; x++)
				pGrabbed->m_pTiles[y * pGrabbed->m_Width + x] = GetTile(r.x + x, r.y + y);
		str_copy(pGrabbed->m_aFilename, pGrabbed->Map()->m_aFilename);
	}

	return 1;
}

void CLayerTiles::FillSelection(bool Empty, CLayer *pBrush, CUIRect Rect)
{
	if(m_Readonly || (!Empty && pBrush->m_Type != LAYERTYPE_TILES))
		return;

	Snap(&Rect);

	int sx = ConvertX(Rect.x);
	int sy = ConvertY(Rect.y);
	int w = ConvertX(Rect.w);
	int h = ConvertY(Rect.h);

	CLayerTiles *pLt = static_cast<CLayerTiles *>(pBrush);

	bool Destructive = Editor()->m_BrushDrawDestructive || Empty || pLt->IsEmpty();

	for(int y = 0; y < h; y++)
	{
		for(int x = 0; x < w; x++)
		{
			int fx = x + sx;
			int fy = y + sy;

			if(fx < 0 || fx >= m_Width || fy < 0 || fy >= m_Height)
				continue;

			bool HasTile = GetTile(fx, fy).m_Index;
			if(!Empty && pLt->GetTile(x % pLt->m_Width, y % pLt->m_Height).m_Index == TILE_THROUGH_CUT)
			{
				if(m_HasGame && Map()->m_pFrontLayer)
				{
					HasTile = HasTile || Map()->m_pFrontLayer->GetTile(fx, fy).m_Index;
				}
				else if(m_HasFront)
				{
					HasTile = HasTile || Map()->m_pGameLayer->GetTile(fx, fy).m_Index;
				}
			}

			if(!Destructive && HasTile)
				continue;

			SetTile(fx, fy, Empty ? CTile{TILE_AIR} : pLt->m_pTiles[(y * pLt->m_Width + x % pLt->m_Width) % (pLt->m_Width * pLt->m_Height)]);
		}
	}
	FlagModified(sx, sy, w, h);
}

void CLayerTiles::BrushDraw(CLayer *pBrush, vec2 WorldPos)
{
	if(m_Readonly)
		return;

	CLayerTiles *pTileLayer = static_cast<CLayerTiles *>(pBrush);
	int sx = ConvertX(WorldPos.x);
	int sy = ConvertY(WorldPos.y);

	bool Destructive = Editor()->m_BrushDrawDestructive || pTileLayer->IsEmpty();

	for(int y = 0; y < pTileLayer->m_Height; y++)
		for(int x = 0; x < pTileLayer->m_Width; x++)
		{
			int fx = x + sx;
			int fy = y + sy;

			if(fx < 0 || fx >= m_Width || fy < 0 || fy >= m_Height)
				continue;

			bool HasTile = GetTile(fx, fy).m_Index;
			if(pTileLayer->CLayerTiles::GetTile(x, y).m_Index == TILE_THROUGH_CUT)
			{
				if(m_HasGame && Map()->m_pFrontLayer)
				{
					HasTile = HasTile || Map()->m_pFrontLayer->GetTile(fx, fy).m_Index;
				}
				else if(m_HasFront)
				{
					HasTile = HasTile || Map()->m_pGameLayer->GetTile(fx, fy).m_Index;
				}
			}

			if(!Destructive && HasTile)
				continue;

			SetTile(fx, fy, pTileLayer->CLayerTiles::GetTile(x, y));
		}

	FlagModified(sx, sy, pTileLayer->m_Width, pTileLayer->m_Height);
}

void CLayerTiles::BrushFill(CLayer *pBrush, vec2 WorldPos)
{
	if(m_Readonly || pBrush->m_Type != LAYERTYPE_TILES)
		return;

	CLayerTiles *pBrushLayer = static_cast<CLayerTiles *>(pBrush);
	if(pBrushLayer->m_Width <= 0 || pBrushLayer->m_Height <= 0)
		return;

	int sx = ConvertX(WorldPos.x);
	int sy = ConvertY(WorldPos.y);
	if(sx < 0 || sx >= m_Width || sy < 0 || sy >= m_Height)
		return;

	const int TargetIndex = GetTile(sx, sy).m_Index;

	std::vector<bool> Visited((size_t)m_Width * m_Height, false);
	std::vector<std::pair<int, int>> Stack;
	std::vector<std::pair<int, int>> Cells;
	Stack.emplace_back(sx, sy);
	Visited[(size_t)sy * m_Width + sx] = true;

	const int aDx[] = {1, -1, 0, 0};
	const int aDy[] = {0, 0, 1, -1};

	while(!Stack.empty())
	{
		std::pair<int, int> Cell = Stack.back();
		Stack.pop_back();
		Cells.push_back(Cell);
		for(int i = 0; i < 4; ++i)
		{
			int nx = Cell.first + aDx[i];
			int ny = Cell.second + aDy[i];
			if(nx < 0 || nx >= m_Width || ny < 0 || ny >= m_Height)
				continue;
			size_t Idx = (size_t)ny * m_Width + nx;
			if(Visited[Idx])
				continue;
			if(GetTile(nx, ny).m_Index != TargetIndex)
				continue;
			Visited[Idx] = true;
			Stack.emplace_back(nx, ny);
		}
	}

	if(Cells.empty())
		return;

	const bool SavedDestructive = Editor()->m_BrushDrawDestructive;
	Editor()->m_BrushDrawDestructive = true;

	const int BrushW = pBrushLayer->m_Width;
	const int BrushH = pBrushLayer->m_Height;

	if(BrushW == 1 && BrushH == 1)
	{
		for(const auto &Cell : Cells)
			BrushDraw(pBrush, vec2(Cell.first * 32.0f + 16.0f, Cell.second * 32.0f + 16.0f));
	}
	else
	{
		// Multi-tile brush: tile pattern across the filled cells, world-grid aligned
		// We create a 1x1 brush of the same derived type, mutate its single cell, and let
		// the existing BrushDraw virtual dispatch handle special tele/switch/speedup/tune data.
		std::shared_ptr<CLayerTiles> pTempBrush;
		if(pBrushLayer->m_HasTele)
			pTempBrush = std::make_shared<CLayerTele>(Map(), 1, 1);
		else if(pBrushLayer->m_HasSwitch)
			pTempBrush = std::make_shared<CLayerSwitch>(Map(), 1, 1);
		else if(pBrushLayer->m_HasSpeedup)
			pTempBrush = std::make_shared<CLayerSpeedup>(Map(), 1, 1);
		else if(pBrushLayer->m_HasTune)
			pTempBrush = std::make_shared<CLayerTune>(Map(), 1, 1);
		else
			pTempBrush = std::make_shared<CLayerTiles>(Map(), 1, 1);

		pTempBrush->m_HasGame = pBrushLayer->m_HasGame;
		pTempBrush->m_HasFront = pBrushLayer->m_HasFront;
		pTempBrush->m_HasTele = pBrushLayer->m_HasTele;
		pTempBrush->m_HasSwitch = pBrushLayer->m_HasSwitch;
		pTempBrush->m_HasSpeedup = pBrushLayer->m_HasSpeedup;
		pTempBrush->m_HasTune = pBrushLayer->m_HasTune;
		pTempBrush->m_Image = pBrushLayer->m_Image;
		str_copy(pTempBrush->m_aFilename, pBrushLayer->m_aFilename);

		const int ShiftX = Editor()->m_FillShiftX;
		const int ShiftY = Editor()->m_FillShiftY;

		for(const auto &Cell : Cells)
		{
			int fx = Cell.first;
			int fy = Cell.second;
			int bx = ((fx + ShiftX) % BrushW + BrushW) % BrushW;
			int by = ((fy + ShiftY) % BrushH + BrushH) % BrushH;
			const int BrushIdx = by * BrushW + bx;

			pTempBrush->m_pTiles[0] = pBrushLayer->m_pTiles[BrushIdx];
			if(pBrushLayer->m_HasTele)
				static_cast<CLayerTele *>(pTempBrush.get())->m_pTeleTile[0] = static_cast<CLayerTele *>(pBrushLayer)->m_pTeleTile[BrushIdx];
			else if(pBrushLayer->m_HasSwitch)
				static_cast<CLayerSwitch *>(pTempBrush.get())->m_pSwitchTile[0] = static_cast<CLayerSwitch *>(pBrushLayer)->m_pSwitchTile[BrushIdx];
			else if(pBrushLayer->m_HasSpeedup)
				static_cast<CLayerSpeedup *>(pTempBrush.get())->m_pSpeedupTile[0] = static_cast<CLayerSpeedup *>(pBrushLayer)->m_pSpeedupTile[BrushIdx];
			else if(pBrushLayer->m_HasTune)
				static_cast<CLayerTune *>(pTempBrush.get())->m_pTuneTile[0] = static_cast<CLayerTune *>(pBrushLayer)->m_pTuneTile[BrushIdx];

			BrushDraw(pTempBrush.get(), vec2(fx * 32.0f + 16.0f, fy * 32.0f + 16.0f));
		}
	}

	Editor()->m_BrushDrawDestructive = SavedDestructive;
}

static void RasterizeThickLineCells(double Ax, double Ay, double Bx, double By, int Thickness, std::vector<std::pair<int, int>> &Out)
{
	const double Dx = Bx - Ax;
	const double Dy = By - Ay;
	const double Length = std::sqrt(Dx * Dx + Dy * Dy);
	const int Samples = std::max(8, (int)(Length * 2.0));
	const int Half = (Thickness - 1) / 2;
	for(int s = 0; s <= Samples; ++s)
	{
		const double t = (double)s / (double)Samples;
		const int Tx = (int)std::floor(Ax + Dx * t);
		const int Ty = (int)std::floor(Ay + Dy * t);
		for(int Oy = -Half; Oy <= Half; ++Oy)
			for(int Ox = -Half; Ox <= Half; ++Ox)
				Out.emplace_back(Tx + Ox, Ty + Oy);
	}
}

static void ScanlineFillPolygon(const std::vector<vec2> &Verts, int Y1, int Y2, std::vector<std::pair<int, int>> &Out)
{
	if(Verts.size() < 3)
		return;
	for(int y = Y1; y <= Y2; ++y)
	{
		std::vector<float> Xs;
		const float YF = (float)y + 0.5f;
		for(size_t e = 0; e < Verts.size(); ++e)
		{
			const vec2 &A = Verts[e];
			const vec2 &B = Verts[(e + 1) % Verts.size()];
			if((A.y <= YF && B.y > YF) || (B.y <= YF && A.y > YF))
			{
				if(A.y == B.y)
					continue;
				const float t = (YF - A.y) / (B.y - A.y);
				Xs.push_back(A.x + t * (B.x - A.x));
			}
		}
		std::sort(Xs.begin(), Xs.end());
		for(size_t i = 0; i + 1 < Xs.size(); i += 2)
		{
			const int x0 = (int)std::floor(Xs[i]);
			const int x1 = (int)std::floor(Xs[i + 1]);
			for(int x = x0; x <= x1; ++x)
				Out.emplace_back(x, y);
		}
	}
}

void CLayerTiles::CollectShapeCells(int X1, int Y1, int X2, int Y2, int Kind, bool Filled, int Thickness, std::vector<std::pair<int, int>> &Out, int NgonSides)
{
	if(X1 > X2)
		std::swap(X1, X2);
	if(Y1 > Y2)
		std::swap(Y1, Y2);
	const int W = X2 - X1 + 1;
	const int H = Y2 - Y1 + 1;
	if(W <= 0 || H <= 0)
		return;

	Thickness = std::max(1, Thickness);
	const int ShortSide = std::min(W, H);
	const int MaxThickness = (ShortSide + 1) / 2;
	Thickness = std::min(Thickness, MaxThickness);

	if(Kind == 0) // SHAPE_RECT
	{
		if(Filled)
		{
			Out.reserve(Out.size() + (size_t)W * H);
			for(int y = Y1; y <= Y2; ++y)
				for(int x = X1; x <= X2; ++x)
					Out.emplace_back(x, y);
		}
		else
		{
			for(int y = Y1; y <= Y2; ++y)
			{
				const int DyT = y - Y1;
				const int DyB = Y2 - y;
				for(int x = X1; x <= X2; ++x)
				{
					const int DxL = x - X1;
					const int DxR = X2 - x;
					if(DxL < Thickness || DxR < Thickness || DyT < Thickness || DyB < Thickness)
						Out.emplace_back(x, y);
				}
			}
		}
	}
	else if(Kind == 1) // SHAPE_ELLIPSE
	{
		const double Cx = (X1 + X2) * 0.5;
		const double Cy = (Y1 + Y2) * 0.5;
		const double Rx = (X2 - X1) * 0.5 + 0.5;
		const double Ry = (Y2 - Y1) * 0.5 + 0.5;
		const double RxIn = std::max(0.5, Rx - Thickness);
		const double RyIn = std::max(0.5, Ry - Thickness);
		const double RxSq = Rx * Rx;
		const double RySq = Ry * Ry;
		const double RxInSq = RxIn * RxIn;
		const double RyInSq = RyIn * RyIn;
		for(int y = Y1; y <= Y2; ++y)
		{
			const double Dy = (y + 0.5) - (Cy + 0.5);
			const double DySq = Dy * Dy;
			for(int x = X1; x <= X2; ++x)
			{
				const double Dx = (x + 0.5) - (Cx + 0.5);
				const double DxSq = Dx * Dx;
				const bool InOuter = DxSq / RxSq + DySq / RySq <= 1.0;
				if(!InOuter)
					continue;
				if(Filled)
				{
					Out.emplace_back(x, y);
				}
				else
				{
					const bool InInner = DxSq / RxInSq + DySq / RyInSq <= 1.0;
					if(!InInner)
						Out.emplace_back(x, y);
				}
			}
		}
	}
	else if(Kind == 2) // SHAPE_TRIANGLE
	{
		// Isoceles triangle pointing up, fitted to bbox.
		std::vector<vec2> Verts = {
			vec2((X1 + X2) * 0.5f + 0.5f, (float)Y1 + 0.5f),
			vec2((float)X1 + 0.5f, (float)Y2 + 0.5f),
			vec2((float)X2 + 0.5f, (float)Y2 + 0.5f),
		};
		if(Filled)
		{
			ScanlineFillPolygon(Verts, Y1, Y2, Out);
		}
		else
		{
			for(size_t e = 0; e < Verts.size(); ++e)
			{
				const vec2 &A = Verts[e];
				const vec2 &B = Verts[(e + 1) % Verts.size()];
				RasterizeThickLineCells(A.x, A.y, B.x, B.y, Thickness, Out);
			}
		}
	}
	else if(Kind == 3) // SHAPE_NGON
	{
		int Sides = std::max(3, NgonSides);
		std::vector<vec2> Verts(Sides);
		const float Cx = (X1 + X2) * 0.5f + 0.5f;
		const float Cy = (Y1 + Y2) * 0.5f + 0.5f;
		const float Rx = (X2 - X1) * 0.5f;
		const float Ry = (Y2 - Y1) * 0.5f;
		for(int i = 0; i < Sides; ++i)
		{
			const float Theta = 2.0f * pi * (float)i / (float)Sides - pi * 0.5f;
			Verts[i] = vec2(Cx + Rx * std::cos(Theta), Cy + Ry * std::sin(Theta));
		}
		if(Filled)
		{
			ScanlineFillPolygon(Verts, Y1, Y2, Out);
		}
		else
		{
			for(int i = 0; i < Sides; ++i)
			{
				const vec2 &A = Verts[i];
				const vec2 &B = Verts[(i + 1) % Sides];
				RasterizeThickLineCells(A.x, A.y, B.x, B.y, Thickness, Out);
			}
		}
	}
}

void CLayerTiles::BrushShape(CLayer *pBrush, int X1, int Y1, int X2, int Y2, int Kind, bool Filled, int Thickness, int NgonSides)
{
	if(m_Readonly || pBrush->m_Type != LAYERTYPE_TILES)
		return;

	CLayerTiles *pBrushLayer = static_cast<CLayerTiles *>(pBrush);
	if(pBrushLayer->m_Width != 1 || pBrushLayer->m_Height != 1)
		return;

	std::vector<std::pair<int, int>> Cells;
	CollectShapeCells(X1, Y1, X2, Y2, Kind, Filled, Thickness, Cells, NgonSides);
	if(Cells.empty())
		return;

	const bool SavedDestructive = Editor()->m_BrushDrawDestructive;
	Editor()->m_BrushDrawDestructive = true;
	for(const auto &Cell : Cells)
	{
		if(Cell.first < 0 || Cell.first >= m_Width || Cell.second < 0 || Cell.second >= m_Height)
			continue;
		BrushDraw(pBrush, vec2(Cell.first * 32.0f + 16.0f, Cell.second * 32.0f + 16.0f));
	}
	Editor()->m_BrushDrawDestructive = SavedDestructive;
}

void CLayerTiles::BrushFlipX()
{
	BrushFlipXImpl(m_pTiles);

	if(m_HasTele || m_HasSpeedup || m_HasTune)
		return;

	bool Rotate = !(m_HasGame || m_HasFront || m_HasSwitch) || Editor()->IsAllowPlaceUnusedTiles();
	for(int y = 0; y < m_Height; y++)
		for(int x = 0; x < m_Width; x++)
			if(!Rotate && !IsRotatableTile(m_pTiles[y * m_Width + x].m_Index))
				m_pTiles[y * m_Width + x].m_Flags = 0;
			else
				m_pTiles[y * m_Width + x].m_Flags ^= (m_pTiles[y * m_Width + x].m_Flags & TILEFLAG_ROTATE) ? TILEFLAG_YFLIP : TILEFLAG_XFLIP;
}

void CLayerTiles::BrushFlipY()
{
	BrushFlipYImpl(m_pTiles);

	if(m_HasTele || m_HasSpeedup || m_HasTune)
		return;

	bool Rotate = !(m_HasGame || m_HasFront || m_HasSwitch) || Editor()->IsAllowPlaceUnusedTiles();
	for(int y = 0; y < m_Height; y++)
		for(int x = 0; x < m_Width; x++)
			if(!Rotate && !IsRotatableTile(m_pTiles[y * m_Width + x].m_Index))
				m_pTiles[y * m_Width + x].m_Flags = 0;
			else
				m_pTiles[y * m_Width + x].m_Flags ^= (m_pTiles[y * m_Width + x].m_Flags & TILEFLAG_ROTATE) ? TILEFLAG_XFLIP : TILEFLAG_YFLIP;
}

void CLayerTiles::BrushRotate(float Amount)
{
	int Rotation = (round_to_int(360.0f * Amount / (pi * 2)) / 90) % 4; // 0=0°, 1=90°, 2=180°, 3=270°
	if(Rotation < 0)
		Rotation += 4;

	if(Rotation == 1 || Rotation == 3)
	{
		// 90° rotation
		CTile *pTempData = new CTile[m_Width * m_Height];
		mem_copy(pTempData, m_pTiles, (size_t)m_Width * m_Height * sizeof(CTile));
		CTile *pDst = m_pTiles;
		bool Rotate = !(m_HasGame || m_HasFront) || Editor()->IsAllowPlaceUnusedTiles();
		for(int x = 0; x < m_Width; ++x)
			for(int y = m_Height - 1; y >= 0; --y, ++pDst)
			{
				*pDst = pTempData[y * m_Width + x];
				if(!Rotate && !IsRotatableTile(pDst->m_Index))
					pDst->m_Flags = 0;
				else
				{
					if(pDst->m_Flags & TILEFLAG_ROTATE)
						pDst->m_Flags ^= (TILEFLAG_YFLIP | TILEFLAG_XFLIP);
					pDst->m_Flags ^= TILEFLAG_ROTATE;
				}
			}

		std::swap(m_Width, m_Height);
		delete[] pTempData;
	}

	if(Rotation == 2 || Rotation == 3)
	{
		BrushFlipX();
		BrushFlipY();
	}
}

static bool SelectPointInPolygon(vec2 P, const std::vector<vec2> &vPoly)
{
	bool Inside = false;
	for(size_t i = 0, j = vPoly.size() - 1; i < vPoly.size(); j = i++)
	{
		if(((vPoly[i].y > P.y) != (vPoly[j].y > P.y)) &&
			(P.x < (vPoly[j].x - vPoly[i].x) * (P.y - vPoly[i].y) / (vPoly[j].y - vPoly[i].y) + vPoly[i].x))
			Inside = !Inside;
	}
	return Inside;
}

int CLayerTiles::BrushGrabFreehand(CLayerGroup *pBrush, const std::vector<vec2> &vPolygon)
{
	if(vPolygon.size() < 3)
		return 0;

	// Fall back to rectangle grab for special DDRace tile layers
	if(m_HasTele || m_HasSpeedup || m_HasSwitch || m_HasTune)
	{
		float MinX = vPolygon[0].x, MaxX = vPolygon[0].x;
		float MinY = vPolygon[0].y, MaxY = vPolygon[0].y;
		for(const auto &P : vPolygon)
		{
			MinX = minimum(MinX, P.x);
			MaxX = maximum(MaxX, P.x);
			MinY = minimum(MinY, P.y);
			MaxY = maximum(MaxY, P.y);
		}
		return BrushGrab(pBrush, {MinX, MinY, MaxX - MinX, MaxY - MinY});
	}

	// Bounding rect of polygon
	float MinX = vPolygon[0].x, MaxX = vPolygon[0].x;
	float MinY = vPolygon[0].y, MaxY = vPolygon[0].y;
	for(const auto &P : vPolygon)
	{
		MinX = minimum(MinX, P.x);
		MaxX = maximum(MaxX, P.x);
		MinY = minimum(MinY, P.y);
		MaxY = maximum(MaxY, P.y);
	}

	CIntRect r;
	Convert({MinX, MinY, MaxX - MinX, MaxY - MinY}, &r);
	Clamp(&r);
	if(!r.w || !r.h)
		return 0;

	std::shared_ptr<CLayerTiles> pGrabbed = std::make_shared<CLayerTiles>(pBrush->Map(), r.w, r.h);
	InitGrabbedLayer(pGrabbed, this);

	for(int y = 0; y < r.h; y++)
	{
		for(int x = 0; x < r.w; x++)
		{
			// World-space center of this tile
			vec2 TileCenter((r.x + x + 0.5f) * 32.0f, (r.y + y + 0.5f) * 32.0f);
			if(SelectPointInPolygon(TileCenter, vPolygon))
				pGrabbed->m_pTiles[y * r.w + x] = GetTile(r.x + x, r.y + y);
			// else: tile stays zero/empty (hole in selection)
		}
	}

	pBrush->AddLayer(pGrabbed);
	return 1;
}

void CLayerTiles::BrushRotateArbitrary(const std::vector<CTile> &vOrigTiles, int OrigW, int OrigH, float Angle)
{
	if(vOrigTiles.empty() || OrigW <= 0 || OrigH <= 0)
		return;

	const float CosA = std::cos(Angle);
	const float SinA = std::sin(Angle);

	// Compute rotated bounding box from original corners (centered at origin)
	const float HW = OrigW * 0.5f;
	const float HH = OrigH * 0.5f;
	const float Corners[4][2] = {{-HW, -HH}, {HW, -HH}, {HW, HH}, {-HW, HH}};
	float MinX = 1e9f, MaxX = -1e9f, MinY = 1e9f, MaxY = -1e9f;
	for(const auto &C : Corners)
	{
		float Rx = C[0] * CosA - C[1] * SinA;
		float Ry = C[0] * SinA + C[1] * CosA;
		MinX = minimum(MinX, Rx);
		MaxX = maximum(MaxX, Rx);
		MinY = minimum(MinY, Ry);
		MaxY = maximum(MaxY, Ry);
	}

	const int OutW = maximum(1, (int)std::ceil(MaxX - MinX));
	const int OutH = maximum(1, (int)std::ceil(MaxY - MinY));

	CTile *pNew = new CTile[OutW * OutH];
	mem_zero(pNew, (size_t)OutW * OutH * sizeof(CTile));

	// Inverse rotation coefficients (angle negated)
	const float ICosA = CosA; // cos(-a) = cos(a)
	const float ISinA = -SinA; // sin(-a) = -sin(a)

	for(int Oy = 0; Oy < OutH; Oy++)
	{
		for(int Ox = 0; Ox < OutW; Ox++)
		{
			// Center-relative position in output space
			float Cx = Ox + 0.5f - OutW * 0.5f;
			float Cy = Oy + 0.5f - OutH * 0.5f;
			// Inverse rotate back to source space
			float Sx = Cx * ICosA - Cy * ISinA + HW;
			float Sy = Cx * ISinA + Cy * ICosA + HH;
			int Ix = (int)Sx;
			int Iy = (int)Sy;
			if(Ix >= 0 && Ix < OrigW && Iy >= 0 && Iy < OrigH)
				pNew[Oy * OutW + Ox] = vOrigTiles[Iy * OrigW + Ix];
		}
	}

	delete[] m_pTiles;
	m_pTiles = pNew;
	m_Width = OutW;
	m_Height = OutH;
}

void CLayerTiles::CollectDonutCells(float CxWorld, float CyWorld, float OuterRx, float OuterRy, float InnerRx, float InnerRy, std::vector<std::pair<int, int>> &Out)
{
	if(OuterRx <= 0.0f || OuterRy <= 0.0f)
		return;

	const int TxMin = (int)std::floor((CxWorld - OuterRx) / 32.0f);
	const int TxMax = (int)std::ceil((CxWorld + OuterRx) / 32.0f);
	const int TyMin = (int)std::floor((CyWorld - OuterRy) / 32.0f);
	const int TyMax = (int)std::ceil((CyWorld + OuterRy) / 32.0f);

	const float RxSq = OuterRx * OuterRx;
	const float RySq = OuterRy * OuterRy;
	const float RxInSq = InnerRx * InnerRx;
	const float RyInSq = InnerRy * InnerRy;

	for(int Ty = TyMin; Ty <= TyMax; ++Ty)
	{
		for(int Tx = TxMin; Tx <= TxMax; ++Tx)
		{
			float Dx = (Tx + 0.5f) * 32.0f - CxWorld;
			float Dy = (Ty + 0.5f) * 32.0f - CyWorld;
			bool InOuter = Dx * Dx / RxSq + Dy * Dy / RySq <= 1.0f;
			bool InInner = RxInSq > 0.0f && RyInSq > 0.0f && Dx * Dx / RxInSq + Dy * Dy / RyInSq <= 1.0f;
			if(InOuter && !InInner)
				Out.emplace_back(Tx, Ty);
		}
	}
}

void CLayerTiles::BrushDonut(CLayer *pBrush, float CxWorld, float CyWorld, float OuterRx, float OuterRy, float InnerRx, float InnerRy)
{
	if(!pBrush)
		return;
	auto *pBrushTiles = static_cast<CLayerTiles *>(pBrush);
	if(pBrushTiles->m_Width == 0 || pBrushTiles->m_Height == 0)
		return;

	std::vector<std::pair<int, int>> Cells;
	CollectDonutCells(CxWorld, CyWorld, OuterRx, OuterRy, InnerRx, InnerRy, Cells);

	const CTile &BrushTile = pBrushTiles->m_pTiles[0];
	for(const auto &C : Cells)
	{
		if(C.first >= 0 && C.first < m_Width && C.second >= 0 && C.second < m_Height)
			SetTile(C.first, C.second, BrushTile);
	}
}

std::shared_ptr<CLayer> CLayerTiles::Duplicate() const
{
	return std::make_shared<CLayerTiles>(*this);
}

const char *CLayerTiles::TypeName() const
{
	return "tiles";
}

void CLayerTiles::Resize(int NewW, int NewH)
{
	CTile *pNewData = new CTile[NewW * NewH];
	mem_zero(pNewData, (size_t)NewW * NewH * sizeof(CTile));

	// copy old data
	for(int y = 0; y < minimum(NewH, m_Height); y++)
		mem_copy(&pNewData[y * NewW], &m_pTiles[y * m_Width], minimum(m_Width, NewW) * sizeof(CTile));

	// replace old
	delete[] m_pTiles;
	m_pTiles = pNewData;
	m_Width = NewW;
	m_Height = NewH;

	// resize tele layer if available
	if(m_HasGame && Map()->m_pTeleLayer && (Map()->m_pTeleLayer->m_Width != NewW || Map()->m_pTeleLayer->m_Height != NewH))
		Map()->m_pTeleLayer->Resize(NewW, NewH);

	// resize speedup layer if available
	if(m_HasGame && Map()->m_pSpeedupLayer && (Map()->m_pSpeedupLayer->m_Width != NewW || Map()->m_pSpeedupLayer->m_Height != NewH))
		Map()->m_pSpeedupLayer->Resize(NewW, NewH);

	// resize front layer
	if(m_HasGame && Map()->m_pFrontLayer && (Map()->m_pFrontLayer->m_Width != NewW || Map()->m_pFrontLayer->m_Height != NewH))
		Map()->m_pFrontLayer->Resize(NewW, NewH);

	// resize switch layer if available
	if(m_HasGame && Map()->m_pSwitchLayer && (Map()->m_pSwitchLayer->m_Width != NewW || Map()->m_pSwitchLayer->m_Height != NewH))
		Map()->m_pSwitchLayer->Resize(NewW, NewH);

	// resize tune layer if available
	if(m_HasGame && Map()->m_pTuneLayer && (Map()->m_pTuneLayer->m_Width != NewW || Map()->m_pTuneLayer->m_Height != NewH))
		Map()->m_pTuneLayer->Resize(NewW, NewH);
}

void CLayerTiles::Shift(EShiftDirection Direction)
{
	ShiftImpl(m_pTiles, Direction, Map()->m_ShiftBy);
}

void CLayerTiles::ShowInfo()
{
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	Graphics()->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);
	Graphics()->TextureSet(Editor()->Client()->GetDebugFont());
	Graphics()->QuadsBegin();

	int StartY = maximum(0, (int)(ScreenY0 / 32.0f) - 1);
	int StartX = maximum(0, (int)(ScreenX0 / 32.0f) - 1);
	int EndY = minimum((int)(ScreenY1 / 32.0f) + 1, m_Height);
	int EndX = minimum((int)(ScreenX1 / 32.0f) + 1, m_Width);

	for(int y = StartY; y < EndY; y++)
		for(int x = StartX; x < EndX; x++)
		{
			int c = x + y * m_Width;
			if(m_pTiles[c].m_Index)
			{
				char aBuf[4];
				if(Editor()->m_ShowTileInfo == CEditor::SHOW_TILE_HEXADECIMAL)
				{
					str_hex(aBuf, sizeof(aBuf), &m_pTiles[c].m_Index, 1);
					aBuf[2] = '\0'; // would otherwise be a space
				}
				else
				{
					str_format(aBuf, sizeof(aBuf), "%d", m_pTiles[c].m_Index);
				}
				Graphics()->QuadsText(x * 32, y * 32, 16.0f, aBuf);

				char aFlags[4] = {m_pTiles[c].m_Flags & TILEFLAG_XFLIP ? 'X' : ' ',
					m_pTiles[c].m_Flags & TILEFLAG_YFLIP ? 'Y' : ' ',
					m_pTiles[c].m_Flags & TILEFLAG_ROTATE ? 'R' : ' ',
					0};
				Graphics()->QuadsText(x * 32, y * 32 + 16, 16.0f, aFlags);
			}
			x += m_pTiles[c].m_Skip;
		}

	Graphics()->QuadsEnd();
	Graphics()->MapScreen(ScreenX0, ScreenY0, ScreenX1, ScreenY1);
}

void CLayerTiles::FillGameTiles(EGameTileOp Fill)
{
	if(!CanFillGameTiles())
		return;

	auto GameTileOpToIndex = [](EGameTileOp Op) -> int {
		switch(Op)
		{
		case EGameTileOp::AIR: return TILE_AIR;
		case EGameTileOp::HOOKABLE: return TILE_SOLID;
		case EGameTileOp::DEATH: return TILE_DEATH;
		case EGameTileOp::UNHOOKABLE: return TILE_NOHOOK;
		case EGameTileOp::HOOKTHROUGH: return TILE_THROUGH_CUT;
		case EGameTileOp::FREEZE: return TILE_FREEZE;
		case EGameTileOp::UNFREEZE: return TILE_UNFREEZE;
		case EGameTileOp::DEEP_FREEZE: return TILE_DFREEZE;
		case EGameTileOp::DEEP_UNFREEZE: return TILE_DUNFREEZE;
		case EGameTileOp::BLUE_CHECK_TELE: return TILE_TELECHECKIN;
		case EGameTileOp::RED_CHECK_TELE: return TILE_TELECHECKINEVIL;
		case EGameTileOp::LIVE_FREEZE: return TILE_LFREEZE;
		case EGameTileOp::LIVE_UNFREEZE: return TILE_LUNFREEZE;
		default: return -1;
		}
	};

	int Result = GameTileOpToIndex(Fill);
	if(Result > -1)
	{
		std::shared_ptr<CLayerGroup> pGroup = Map()->m_vpGroups[Map()->m_SelectedGroup];
		m_FillGameTile = Result;
		const int OffsetX = -pGroup->m_OffsetX / 32;
		const int OffsetY = -pGroup->m_OffsetY / 32;

		std::vector<std::shared_ptr<IEditorAction>> vpActions;
		std::shared_ptr<CLayerTiles> pGLayer = Map()->m_pGameLayer;
		int GameLayerIndex = std::find(Map()->m_pGameGroup->m_vpLayers.begin(), Map()->m_pGameGroup->m_vpLayers.end(), pGLayer) - Map()->m_pGameGroup->m_vpLayers.begin();
		int GameGroupIndex = std::find(Map()->m_vpGroups.begin(), Map()->m_vpGroups.end(), Map()->m_pGameGroup) - Map()->m_vpGroups.begin();

		if(Result != TILE_TELECHECKIN && Result != TILE_TELECHECKINEVIL)
		{
			if(pGLayer->m_Width < m_Width + OffsetX || pGLayer->m_Height < m_Height + OffsetY)
			{
				std::map<int, std::shared_ptr<CLayer>> SavedLayers;
				SavedLayers[LAYERTYPE_TILES] = pGLayer->Duplicate();
				SavedLayers[LAYERTYPE_GAME] = SavedLayers[LAYERTYPE_TILES];

				int PrevW = pGLayer->m_Width;
				int PrevH = pGLayer->m_Height;
				const int NewW = pGLayer->m_Width < m_Width + OffsetX ? m_Width + OffsetX : pGLayer->m_Width;
				const int NewH = pGLayer->m_Height < m_Height + OffsetY ? m_Height + OffsetY : pGLayer->m_Height;
				pGLayer->Resize(NewW, NewH);
				vpActions.push_back(std::make_shared<CEditorActionEditLayerTilesProp>(Map(), GameGroupIndex, GameLayerIndex, ETilesProp::WIDTH, PrevW, NewW));
				const std::shared_ptr<CEditorActionEditLayerTilesProp> &Action1 = std::static_pointer_cast<CEditorActionEditLayerTilesProp>(vpActions[vpActions.size() - 1]);
				vpActions.push_back(std::make_shared<CEditorActionEditLayerTilesProp>(Map(), GameGroupIndex, GameLayerIndex, ETilesProp::HEIGHT, PrevH, NewH));
				const std::shared_ptr<CEditorActionEditLayerTilesProp> &Action2 = std::static_pointer_cast<CEditorActionEditLayerTilesProp>(vpActions[vpActions.size() - 1]);

				Action1->SetSavedLayers(SavedLayers);
				Action2->SetSavedLayers(SavedLayers);
			}

			int Changes = 0;
			for(int y = OffsetY < 0 ? -OffsetY : 0; y < m_Height; y++)
			{
				for(int x = OffsetX < 0 ? -OffsetX : 0; x < m_Width; x++)
				{
					if(GetTile(x, y).m_Index)
					{
						pGLayer->SetTile(x + OffsetX, y + OffsetY, CTile{(unsigned char)Result});
						Changes++;
					}
				}
			}

			vpActions.push_back(std::make_shared<CEditorBrushDrawAction>(Map(), GameGroupIndex));
			char aDisplay[256];
			str_format(aDisplay, sizeof(aDisplay), "Construct '%s' game tiles (x%d)", GAME_TILE_OP_NAMES[(int)Fill], Changes);
			Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionBulk>(Map(), vpActions, aDisplay, true));
		}
		else
		{
			if(!Map()->m_pTeleLayer)
			{
				std::shared_ptr<CLayerTele> pLayer = std::make_shared<CLayerTele>(Map(), m_Width, m_Height);
				Map()->MakeTeleLayer(pLayer);
				Map()->m_pGameGroup->AddLayer(pLayer);

				vpActions.push_back(std::make_shared<CEditorActionAddLayer>(Map(), GameGroupIndex, Map()->m_pGameGroup->m_vpLayers.size() - 1));

				if(m_Width != pGLayer->m_Width || m_Height > pGLayer->m_Height)
				{
					std::map<int, std::shared_ptr<CLayer>> SavedLayers;
					SavedLayers[LAYERTYPE_TILES] = pGLayer->Duplicate();
					SavedLayers[LAYERTYPE_GAME] = SavedLayers[LAYERTYPE_TILES];

					int NewW = pGLayer->m_Width;
					int NewH = pGLayer->m_Height;
					if(m_Width > pGLayer->m_Width)
					{
						NewW = m_Width;
					}
					if(m_Height > pGLayer->m_Height)
					{
						NewH = m_Height;
					}

					int PrevW = pGLayer->m_Width;
					int PrevH = pGLayer->m_Height;
					pLayer->Resize(NewW, NewH);
					vpActions.push_back(std::make_shared<CEditorActionEditLayerTilesProp>(Map(), GameGroupIndex, GameLayerIndex, ETilesProp::WIDTH, PrevW, NewW));
					const std::shared_ptr<CEditorActionEditLayerTilesProp> &Action1 = std::static_pointer_cast<CEditorActionEditLayerTilesProp>(vpActions[vpActions.size() - 1]);
					vpActions.push_back(std::make_shared<CEditorActionEditLayerTilesProp>(Map(), GameGroupIndex, GameLayerIndex, ETilesProp::HEIGHT, PrevH, NewH));
					const std::shared_ptr<CEditorActionEditLayerTilesProp> &Action2 = std::static_pointer_cast<CEditorActionEditLayerTilesProp>(vpActions[vpActions.size() - 1]);

					Action1->SetSavedLayers(SavedLayers);
					Action2->SetSavedLayers(SavedLayers);
				}
			}

			std::shared_ptr<CLayerTele> pTLayer = Map()->m_pTeleLayer;
			int TeleLayerIndex = std::find(Map()->m_pGameGroup->m_vpLayers.begin(), Map()->m_pGameGroup->m_vpLayers.end(), pTLayer) - Map()->m_pGameGroup->m_vpLayers.begin();

			if(pTLayer->m_Width < m_Width + OffsetX || pTLayer->m_Height < m_Height + OffsetY)
			{
				std::map<int, std::shared_ptr<CLayer>> SavedLayers;
				SavedLayers[LAYERTYPE_TILES] = pTLayer->Duplicate();
				SavedLayers[LAYERTYPE_TELE] = SavedLayers[LAYERTYPE_TILES];

				int PrevW = pTLayer->m_Width;
				int PrevH = pTLayer->m_Height;
				int NewW = pTLayer->m_Width < m_Width + OffsetX ? m_Width + OffsetX : pTLayer->m_Width;
				int NewH = pTLayer->m_Height < m_Height + OffsetY ? m_Height + OffsetY : pTLayer->m_Height;
				pTLayer->Resize(NewW, NewH);
				std::shared_ptr<CEditorActionEditLayerTilesProp> Action1, Action2;
				vpActions.push_back(Action1 = std::make_shared<CEditorActionEditLayerTilesProp>(Map(), GameGroupIndex, TeleLayerIndex, ETilesProp::WIDTH, PrevW, NewW));
				vpActions.push_back(Action2 = std::make_shared<CEditorActionEditLayerTilesProp>(Map(), GameGroupIndex, TeleLayerIndex, ETilesProp::HEIGHT, PrevH, NewH));

				Action1->SetSavedLayers(SavedLayers);
				Action2->SetSavedLayers(SavedLayers);
			}

			int Changes = 0;
			for(int y = OffsetY < 0 ? -OffsetY : 0; y < m_Height; y++)
			{
				for(int x = OffsetX < 0 ? -OffsetX : 0; x < m_Width; x++)
				{
					if(GetTile(x, y).m_Index)
					{
						auto TileIndex = (y + OffsetY) * pTLayer->m_Width + x + OffsetX;
						Changes++;

						STeleTileStateChange::SData Previous{
							pTLayer->m_pTeleTile[TileIndex].m_Number,
							pTLayer->m_pTeleTile[TileIndex].m_Type,
							pTLayer->m_pTiles[TileIndex].m_Index};

						pTLayer->m_pTiles[TileIndex].m_Index = TILE_AIR + Result;
						pTLayer->m_pTeleTile[TileIndex].m_Number = 1;
						pTLayer->m_pTeleTile[TileIndex].m_Type = TILE_AIR + Result;

						STeleTileStateChange::SData Current{
							pTLayer->m_pTeleTile[TileIndex].m_Number,
							pTLayer->m_pTeleTile[TileIndex].m_Type,
							pTLayer->m_pTiles[TileIndex].m_Index};

						pTLayer->RecordStateChange(x, y, Previous, Current);
					}
				}
			}

			vpActions.push_back(std::make_shared<CEditorBrushDrawAction>(Map(), GameGroupIndex));
			char aDisplay[256];
			str_format(aDisplay, sizeof(aDisplay), "Construct 'tele' game tiles (x%d)", Changes);
			Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionBulk>(Map(), vpActions, aDisplay, true));
		}
	}
}

bool CLayerTiles::CanFillGameTiles() const
{
	const bool EntitiesLayer = IsEntitiesLayer();
	if(EntitiesLayer)
		return false;

	std::shared_ptr<CLayerGroup> pGroup = Map()->m_vpGroups[Map()->m_SelectedGroup];

	// Game tiles can only be constructed if the layer is relative to the game layer
	return !(pGroup->m_OffsetX % 32) && !(pGroup->m_OffsetY % 32) && pGroup->m_ParallaxX == 100 && pGroup->m_ParallaxY == 100;
}

CUi::EPopupMenuFunctionResult CLayerTiles::RenderProperties(CUIRect *pToolBox)
{
	CUIRect Button;

	const bool EntitiesLayer = IsEntitiesLayer();

	if(CanFillGameTiles())
	{
		pToolBox->HSplitBottom(12.0f, pToolBox, &Button);
		static int s_GameTilesButton = 0;

		auto GameTileToOp = [](int TileIndex) -> EGameTileOp {
			switch(TileIndex)
			{
			case TILE_AIR: return EGameTileOp::AIR;
			case TILE_SOLID: return EGameTileOp::HOOKABLE;
			case TILE_DEATH: return EGameTileOp::DEATH;
			case TILE_NOHOOK: return EGameTileOp::UNHOOKABLE;
			case TILE_THROUGH_CUT: return EGameTileOp::HOOKTHROUGH;
			case TILE_FREEZE: return EGameTileOp::FREEZE;
			case TILE_UNFREEZE: return EGameTileOp::UNFREEZE;
			case TILE_DFREEZE: return EGameTileOp::DEEP_FREEZE;
			case TILE_DUNFREEZE: return EGameTileOp::DEEP_UNFREEZE;
			case TILE_TELECHECKIN: return EGameTileOp::BLUE_CHECK_TELE;
			case TILE_TELECHECKINEVIL: return EGameTileOp::RED_CHECK_TELE;
			case TILE_LFREEZE: return EGameTileOp::LIVE_FREEZE;
			case TILE_LUNFREEZE: return EGameTileOp::LIVE_UNFREEZE;
			default: return EGameTileOp::AIR;
			}
		};

		char aBuf[128] = "Game tiles";
		if(m_LiveGameTiles)
		{
			auto TileOp = GameTileToOp(m_FillGameTile);
			if(TileOp != EGameTileOp::AIR)
				str_format(aBuf, sizeof(aBuf), "Game tiles: %s", GAME_TILE_OP_NAMES[(size_t)TileOp]);
		}
		if(Editor()->DoButton_Editor(&s_GameTilesButton, aBuf, 0, &Button, BUTTONFLAG_LEFT, "Construct game tiles from this layer."))
			Editor()->PopupSelectGametileOpInvoke(Editor()->Ui()->MouseX(), Editor()->Ui()->MouseY());
		const int Selected = Editor()->PopupSelectGameTileOpResult();
		FillGameTiles((EGameTileOp)Selected);
	}

	if(Map()->m_pGameLayer.get() != this)
	{
		if(m_Image >= 0 && (size_t)m_Image < Map()->m_vpImages.size() && Map()->m_vpImages[m_Image]->m_AutoMapper.IsLoaded() && m_AutoMapperConfig != -1)
		{
			pToolBox->HSplitBottom(2.0f, pToolBox, nullptr);
			pToolBox->HSplitBottom(12.0f, pToolBox, &Button);
			if(m_Seed != 0)
			{
				CUIRect ButtonAuto;
				Button.VSplitRight(16.0f, &Button, &ButtonAuto);
				Button.VSplitRight(2.0f, &Button, nullptr);
				static int s_AutoMapperButtonAuto = 0;
				if(Editor()->DoButton_Editor(&s_AutoMapperButtonAuto, "A", m_AutoAutoMap, &ButtonAuto, BUTTONFLAG_LEFT, "Automatically run the automapper after modifications."))
				{
					m_AutoAutoMap = !m_AutoAutoMap;
					FlagModified(0, 0, m_Width, m_Height);
					if(!m_TilesHistory.empty()) // Sometimes pressing that button causes the automap to run so we should be able to undo that
					{
						// record undo
						Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionTileChanges>(Map(), Map()->m_SelectedGroup, Map()->m_vSelectedLayers[0], "Auto map", m_TilesHistory));
						ClearHistory();
					}
				}
			}

			static int s_AutoMapperButton = 0;
			if(Editor()->DoButton_Editor(&s_AutoMapperButton, "Automap", 0, &Button, BUTTONFLAG_LEFT, "Run the automapper."))
			{
				Map()->m_vpImages[m_Image]->m_AutoMapper.Proceed(this, Map()->m_pGameLayer.get(), m_AutoMapperReference, m_AutoMapperConfig, m_Seed);
				// record undo
				Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionTileChanges>(Map(), Map()->m_SelectedGroup, Map()->m_vSelectedLayers[0], "Auto map", m_TilesHistory));
				ClearHistory();
				return CUi::POPUP_CLOSE_CURRENT;
			}
		}
	}

	CProperty aProps[] = {
		{"Width", m_Width, PROPTYPE_INT, 2, 100000},
		{"Height", m_Height, PROPTYPE_INT, 2, 100000},
		{"Shift", 0, PROPTYPE_SHIFT, 0, 0},
		{"Shift by", Map()->m_ShiftBy, PROPTYPE_INT, 1, 100000},
		{"Image", m_Image, PROPTYPE_IMAGE, 0, 0},
		{"Color", PackColor(m_Color), PROPTYPE_COLOR, 0, 0},
		{"Color Env", m_ColorEnv + 1, PROPTYPE_ENVELOPE, 0, 0},
		{"Color TO", m_ColorEnvOffset, PROPTYPE_INT, -1000000, 1000000},
		{"Auto Rule", m_AutoMapperConfig, PROPTYPE_AUTOMAPPER, m_Image, 0},
		{"Reference", m_AutoMapperReference, PROPTYPE_AUTOMAPPER_REFERENCE, 0, 0},
		{"Live Gametiles", m_LiveGameTiles, PROPTYPE_BOOL, 0, 1},
		{"Seed", m_Seed, PROPTYPE_INT, 0, 1000000000},
		{nullptr},
	};

	if(EntitiesLayer) // remove the image and color properties if this is a game layer
	{
		aProps[(int)ETilesProp::IMAGE].m_pName = nullptr;
		aProps[(int)ETilesProp::COLOR].m_pName = nullptr;
		aProps[(int)ETilesProp::AUTOMAPPER].m_pName = nullptr;
		aProps[(int)ETilesProp::AUTOMAPPER_REFERENCE].m_pName = nullptr;
	}
	if(m_Image == -1)
	{
		aProps[(int)ETilesProp::AUTOMAPPER].m_pName = nullptr;
		aProps[(int)ETilesProp::AUTOMAPPER_REFERENCE].m_pName = nullptr;
		aProps[(int)ETilesProp::SEED].m_pName = nullptr;
	}

	static int s_aIds[(int)ETilesProp::NUM_PROPS] = {0};
	int NewVal = 0;
	auto [State, Prop] = Editor()->DoPropertiesWithState<ETilesProp>(pToolBox, aProps, s_aIds, &NewVal);

	Map()->m_LayerTilesPropTracker.Begin(this, Prop, State);
	Map()->m_EditorHistory.BeginBulk();

	if(Prop == ETilesProp::WIDTH)
	{
		if(NewVal > 1000 && !Editor()->m_LargeLayerWasWarned)
		{
			Editor()->m_PopupEventType = CEditor::POPEVENT_LARGELAYER;
			Editor()->m_PopupEventActivated = true;
			Editor()->m_LargeLayerWasWarned = true;
		}
		Resize(NewVal, m_Height);
	}
	else if(Prop == ETilesProp::HEIGHT)
	{
		if(NewVal > 1000 && !Editor()->m_LargeLayerWasWarned)
		{
			Editor()->m_PopupEventType = CEditor::POPEVENT_LARGELAYER;
			Editor()->m_PopupEventActivated = true;
			Editor()->m_LargeLayerWasWarned = true;
		}
		Resize(m_Width, NewVal);
	}
	else if(Prop == ETilesProp::SHIFT)
	{
		Shift((EShiftDirection)NewVal);
	}
	else if(Prop == ETilesProp::SHIFT_BY)
	{
		Map()->m_ShiftBy = NewVal;
	}
	else if(Prop == ETilesProp::IMAGE)
	{
		m_Image = NewVal;
		if(NewVal == -1)
		{
			m_Image = -1;
		}
		else
		{
			m_Image = NewVal % Map()->m_vpImages.size();
			m_AutoMapperConfig = -1;

			if(Map()->m_vpImages[m_Image]->m_Width % 16 != 0 || Map()->m_vpImages[m_Image]->m_Height % 16 != 0)
			{
				Editor()->m_PopupEventType = CEditor::POPEVENT_IMAGEDIV16;
				Editor()->m_PopupEventActivated = true;
				m_Image = -1;
			}
		}
	}
	else if(Prop == ETilesProp::COLOR)
	{
		m_Color = UnpackColor(NewVal);
	}
	else if(Prop == ETilesProp::COLOR_ENV)
	{
		int Index = std::clamp(NewVal - 1, -1, (int)Map()->m_vpEnvelopes.size() - 1);
		const int Step = (Index - m_ColorEnv) % 2;
		if(Step != 0)
		{
			for(; Index >= -1 && Index < (int)Map()->m_vpEnvelopes.size(); Index += Step)
			{
				if(Index == -1 || Map()->m_vpEnvelopes[Index]->GetChannels() == 4)
				{
					m_ColorEnv = Index;
					break;
				}
			}
		}
	}
	else if(Prop == ETilesProp::COLOR_ENV_OFFSET)
	{
		m_ColorEnvOffset = NewVal;
	}
	else if(Prop == ETilesProp::SEED)
	{
		m_Seed = NewVal;
	}
	else if(Prop == ETilesProp::AUTOMAPPER)
	{
		if(m_Image >= 0 && Map()->m_vpImages[m_Image]->m_AutoMapper.ConfigNamesNum() > 0 && NewVal >= 0)
			m_AutoMapperConfig = NewVal % Map()->m_vpImages[m_Image]->m_AutoMapper.ConfigNamesNum();
		else
			m_AutoMapperConfig = -1;
	}
	else if(Prop == ETilesProp::AUTOMAPPER_REFERENCE)
	{
		m_AutoMapperReference = NewVal;
	}
	else if(Prop == ETilesProp::LIVE_GAMETILES)
	{
		m_LiveGameTiles = NewVal != 0;
	}

	Map()->m_LayerTilesPropTracker.End(Prop, State);

	// Check if modified property could have an effect on automapper
	if((State == EEditState::END || State == EEditState::ONE_GO) && HasAutomapEffect(Prop))
	{
		FlagModified(0, 0, m_Width, m_Height);

		// Record undo if automapper was ran
		if(m_AutoAutoMap && !m_TilesHistory.empty())
		{
			Map()->m_EditorHistory.RecordAction(std::make_shared<CEditorActionTileChanges>(Map(), Map()->m_SelectedGroup, Map()->m_vSelectedLayers[0], "Auto map", m_TilesHistory));
			ClearHistory();
		}
	}

	// End undo bulk, taking the first action display as the displayed text in the history
	// This is usually the resulting text of the edit layer tiles prop action
	// Since we may also squeeze a tile changes action, we want both to appear as one, thus using a bulk
	Map()->m_EditorHistory.EndBulk(0);

	return CUi::POPUP_KEEP_OPEN;
}

CUi::EPopupMenuFunctionResult CLayerTiles::RenderCommonProperties(SCommonPropState &State, CEditorMap *pEditorMap, CUIRect *pToolbox, std::vector<std::shared_ptr<CLayerTiles>> &vpLayers, std::vector<int> &vLayerIndices)
{
	CEditor *pEditor = pEditorMap->Editor();
	if(State.m_Modified)
	{
		CUIRect Commit;
		pToolbox->HSplitBottom(20.0f, pToolbox, &Commit);
		static int s_CommitButton = 0;
		if(pEditor->DoButton_Editor(&s_CommitButton, "Commit", 0, &Commit, BUTTONFLAG_LEFT, "Apply the changes."))
		{
			bool HasModifiedSize = (State.m_Modified & SCommonPropState::MODIFIED_SIZE) != 0;
			bool HasModifiedColor = (State.m_Modified & SCommonPropState::MODIFIED_COLOR) != 0;

			std::vector<std::shared_ptr<IEditorAction>> vpActions;
			int j = 0;
			int GroupIndex = pEditorMap->m_SelectedGroup;
			for(auto &pLayer : vpLayers)
			{
				int LayerIndex = vLayerIndices[j++];
				if(HasModifiedSize)
				{
					std::map<int, std::shared_ptr<CLayer>> SavedLayers;
					SavedLayers[LAYERTYPE_TILES] = pLayer->Duplicate();
					if(pLayer->m_HasGame || pLayer->m_HasFront || pLayer->m_HasSwitch || pLayer->m_HasSpeedup || pLayer->m_HasTune || pLayer->m_HasTele)
					{ // Need to save all entities layers when any entity layer
						if(pEditorMap->m_pFrontLayer && !pLayer->m_HasFront)
							SavedLayers[LAYERTYPE_FRONT] = pEditorMap->m_pFrontLayer->Duplicate();
						if(pEditorMap->m_pTeleLayer && !pLayer->m_HasTele)
							SavedLayers[LAYERTYPE_TELE] = pEditorMap->m_pTeleLayer->Duplicate();
						if(pEditorMap->m_pSwitchLayer && !pLayer->m_HasSwitch)
							SavedLayers[LAYERTYPE_SWITCH] = pEditorMap->m_pSwitchLayer->Duplicate();
						if(pEditorMap->m_pSpeedupLayer && !pLayer->m_HasSpeedup)
							SavedLayers[LAYERTYPE_SPEEDUP] = pEditorMap->m_pSpeedupLayer->Duplicate();
						if(pEditorMap->m_pTuneLayer && !pLayer->m_HasTune)
							SavedLayers[LAYERTYPE_TUNE] = pEditorMap->m_pTuneLayer->Duplicate();
						if(!pLayer->m_HasGame)
							SavedLayers[LAYERTYPE_GAME] = pEditorMap->m_pGameLayer->Duplicate();
					}

					int PrevW = pLayer->m_Width;
					int PrevH = pLayer->m_Height;
					pLayer->Resize(State.m_Width, State.m_Height);

					if(PrevW != State.m_Width)
					{
						std::shared_ptr<CEditorActionEditLayerTilesProp> pAction;
						vpActions.push_back(pAction = std::make_shared<CEditorActionEditLayerTilesProp>(pEditorMap, GroupIndex, LayerIndex, ETilesProp::WIDTH, PrevW, State.m_Width));
						pAction->SetSavedLayers(SavedLayers);
					}

					if(PrevH != State.m_Height)
					{
						std::shared_ptr<CEditorActionEditLayerTilesProp> pAction;
						vpActions.push_back(pAction = std::make_shared<CEditorActionEditLayerTilesProp>(pEditorMap, GroupIndex, LayerIndex, ETilesProp::HEIGHT, PrevH, State.m_Height));
						pAction->SetSavedLayers(SavedLayers);
					}
				}

				if(HasModifiedColor && !pLayer->IsEntitiesLayer())
				{
					const int PackedColor = PackColor(pLayer->m_Color);
					pLayer->m_Color = UnpackColor(State.m_Color);
					vpActions.push_back(std::make_shared<CEditorActionEditLayerTilesProp>(pEditorMap, GroupIndex, LayerIndex, ETilesProp::COLOR, PackedColor, State.m_Color));
				}

				pLayer->FlagModified(0, 0, pLayer->m_Width, pLayer->m_Height);
			}
			State.m_Modified = 0;

			char aDisplay[256];
			str_format(aDisplay, sizeof(aDisplay), "Edit %d layers common properties: %s", (int)vpLayers.size(), HasModifiedColor && HasModifiedSize ? "color, size" : (HasModifiedColor ? "color" : "size"));
			pEditorMap->m_EditorHistory.RecordAction(std::make_shared<CEditorActionBulk>(pEditorMap, vpActions, aDisplay));
		}
	}
	else
	{
		for(auto &pLayer : vpLayers)
		{
			if(pLayer->m_Width > State.m_Width)
				State.m_Width = pLayer->m_Width;
			if(pLayer->m_Height > State.m_Height)
				State.m_Height = pLayer->m_Height;
		}

		State.m_Color = PackColor(vpLayers[0]->m_Color);
	}

	{
		CUIRect Warning;
		pToolbox->HSplitTop(13.0f, &Warning, pToolbox);
		Warning.HMargin(0.5f, &Warning);

		pEditor->TextRender()->TextColor(ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f));
		SLabelProperties Props;
		Props.m_MaxWidth = Warning.w;
		pEditor->Ui()->DoLabel(&Warning, "Editing multiple layers", 9.0f, TEXTALIGN_ML, Props);
		pEditor->TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
		pToolbox->HSplitTop(2.0f, nullptr, pToolbox);
	}

	CProperty aProps[] = {
		{"Width", State.m_Width, PROPTYPE_INT, 2, 100000},
		{"Height", State.m_Height, PROPTYPE_INT, 2, 100000},
		{"Shift", 0, PROPTYPE_SHIFT, 0, 0},
		{"Shift by", pEditorMap->m_ShiftBy, PROPTYPE_INT, 1, 100000},
		{"Color", State.m_Color, PROPTYPE_COLOR, 0, 0},
		{nullptr},
	};

	static int s_aIds[(int)ETilesCommonProp::NUM_PROPS] = {0};
	int NewVal = 0;
	auto [PropState, Prop] = pEditor->DoPropertiesWithState<ETilesCommonProp>(pToolbox, aProps, s_aIds, &NewVal);

	pEditorMap->m_LayerTilesCommonPropTracker.m_vpLayers = vpLayers;
	pEditorMap->m_LayerTilesCommonPropTracker.m_vLayerIndices = vLayerIndices;

	pEditorMap->m_LayerTilesCommonPropTracker.Begin(nullptr, Prop, PropState);

	if(Prop == ETilesCommonProp::WIDTH)
	{
		if(NewVal > 1000 && !pEditor->m_LargeLayerWasWarned)
		{
			pEditor->m_PopupEventType = CEditor::POPEVENT_LARGELAYER;
			pEditor->m_PopupEventActivated = true;
			pEditor->m_LargeLayerWasWarned = true;
		}
		State.m_Width = NewVal;
	}
	else if(Prop == ETilesCommonProp::HEIGHT)
	{
		if(NewVal > 1000 && !pEditor->m_LargeLayerWasWarned)
		{
			pEditor->m_PopupEventType = CEditor::POPEVENT_LARGELAYER;
			pEditor->m_PopupEventActivated = true;
			pEditor->m_LargeLayerWasWarned = true;
		}
		State.m_Height = NewVal;
	}
	else if(Prop == ETilesCommonProp::SHIFT)
	{
		for(auto &pLayer : vpLayers)
			pLayer->Shift((EShiftDirection)NewVal);
	}
	else if(Prop == ETilesCommonProp::SHIFT_BY)
	{
		pEditorMap->m_ShiftBy = NewVal;
	}
	else if(Prop == ETilesCommonProp::COLOR)
	{
		State.m_Color = NewVal;
	}

	pEditorMap->m_LayerTilesCommonPropTracker.End(Prop, PropState);

	if(PropState == EEditState::END || PropState == EEditState::ONE_GO)
	{
		if(Prop == ETilesCommonProp::WIDTH || Prop == ETilesCommonProp::HEIGHT)
		{
			State.m_Modified |= SCommonPropState::MODIFIED_SIZE;
		}
		else if(Prop == ETilesCommonProp::COLOR)
		{
			State.m_Modified |= SCommonPropState::MODIFIED_COLOR;
		}
	}

	return CUi::POPUP_KEEP_OPEN;
}

void CLayerTiles::FlagModified(int x, int y, int w, int h)
{
	Map()->OnModify();
	if(m_Seed != 0 && m_AutoMapperConfig != -1 && m_AutoAutoMap && m_Image >= 0)
	{
		Map()->m_vpImages[m_Image]->m_AutoMapper.ProceedLocalized(this, Map()->m_pGameLayer.get(), m_AutoMapperReference, m_AutoMapperConfig, m_Seed, x, y, w, h);
	}
}

void CLayerTiles::ModifyImageIndex(const FIndexModifyFunction &IndexModifyFunction)
{
	IndexModifyFunction(&m_Image);
}

void CLayerTiles::ModifyEnvelopeIndex(const FIndexModifyFunction &IndexModifyFunction)
{
	IndexModifyFunction(&m_ColorEnv);
}

void CLayerTiles::ShowPreventUnusedTilesWarning()
{
	if(!Editor()->m_PreventUnusedTilesWasWarned)
	{
		Editor()->m_PopupEventType = CEditor::POPEVENT_PREVENTUNUSEDTILES;
		Editor()->m_PopupEventActivated = true;
		Editor()->m_PreventUnusedTilesWasWarned = true;
	}
}

bool CLayerTiles::HasAutomapEffect(ETilesProp Prop)
{
	switch(Prop)
	{
	case ETilesProp::WIDTH:
	case ETilesProp::HEIGHT:
	case ETilesProp::SHIFT:
	case ETilesProp::IMAGE:
	case ETilesProp::AUTOMAPPER:
	case ETilesProp::SEED:
		return true;
	default:
		return false;
	}
	return false;
}
