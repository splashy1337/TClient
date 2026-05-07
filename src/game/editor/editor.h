/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_EDITOR_EDITOR_H
#define GAME_EDITOR_EDITOR_H

#include "editor_history.h"
#include "editor_server_settings.h"
#include "editor_trackers.h"
#include "editor_ui.h"
#include "font_typer.h"
#include "layer_selector.h"
#include "map_view.h"
#include "quad_art.h"
#include "smooth_value.h"

#include <base/bezier.h>
#include <base/fs.h>

#include <engine/editor.h>
#include <engine/graphics.h>

#include <game/client/ui.h>
#include <game/client/ui_listbox.h>
#include <game/editor/enums.h>
#include <game/editor/file_browser.h>
#include <game/editor/mapitems/envelope.h>
#include <game/editor/mapitems/layer.h>
#include <game/editor/mapitems/layer_front.h>
#include <game/editor/mapitems/layer_game.h>
#include <game/editor/mapitems/layer_group.h>
#include <game/editor/mapitems/layer_quads.h>
#include <game/editor/mapitems/layer_sounds.h>
#include <game/editor/mapitems/layer_speedup.h>
#include <game/editor/mapitems/layer_switch.h>
#include <game/editor/mapitems/layer_tele.h>
#include <game/editor/mapitems/layer_tiles.h>
#include <game/editor/mapitems/layer_tune.h>
#include <game/editor/mapitems/map.h>
#include <game/editor/prompt.h>
#include <game/editor/quick_action.h>
#include <game/map/render_interfaces.h>
#include <game/mapitems.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

template<typename T>
using FDropdownRenderCallback = std::function<void(const T &, char (&aOutput)[128], std::vector<STextColorSplit> &)>;

// CEditor SPECIFIC
enum
{
	MODE_LAYERS = 0,
	MODE_IMAGES,
	MODE_SOUNDS,

	NUM_MODES,
};

enum
{
	DIALOG_NONE = 0,
	DIALOG_FILE,
	DIALOG_MAPSETTINGS_ERROR,
	DIALOG_QUICK_PROMPT,

	// The font typer component sets m_Dialog
	// while it is active to make sure no other component
	// interprets the key presses
	DIALOG_PSEUDO_FONT_TYPER,
};

class CProperty
{
public:
	CProperty(const char *pName, int Value, int Type, int Min, int Max) :
		m_pName(pName), m_Value(Value), m_Type(Type), m_Min(Min), m_Max(Max) {}

	CProperty(std::nullptr_t) :
		m_pName(nullptr), m_Value(0), m_Type(0), m_Min(0), m_Max(0) {}

	const char *m_pName;
	int m_Value;
	int m_Type;
	int m_Min;
	int m_Max;
};

enum
{
	PROPTYPE_NULL = 0,
	PROPTYPE_BOOL,
	PROPTYPE_INT,
	PROPTYPE_ANGLE_SCROLL,
	PROPTYPE_COLOR,
	PROPTYPE_IMAGE,
	PROPTYPE_ENVELOPE,
	PROPTYPE_SHIFT,
	PROPTYPE_SOUND,
	PROPTYPE_AUTOMAPPER,
	PROPTYPE_AUTOMAPPER_REFERENCE,
};

class CEditor : public IEditor, public IEnvelopeEval
{
	class IInput *m_pInput = nullptr;
	class IClient *m_pClient = nullptr;
	class IConfigManager *m_pConfigManager = nullptr;
	class CConfig *m_pConfig = nullptr;
	class IEngine *m_pEngine = nullptr;
	class IGraphics *m_pGraphics = nullptr;
	class ITextRender *m_pTextRender = nullptr;
	class ISound *m_pSound = nullptr;
	class IStorage *m_pStorage = nullptr;
	CRenderMap m_RenderMap;
	CUi m_UI;

	std::vector<std::reference_wrapper<CEditorComponent>> m_vComponents;
	CMapView m_MapView;
	CLayerSelector m_LayerSelector;
	CFileBrowser m_FileBrowser;
	CPrompt m_Prompt;
	CFontTyper m_FontTyper;

	bool m_EditorWasUsedBefore = false;

	IGraphics::CTextureHandle m_EntitiesTexture;

	IGraphics::CTextureHandle m_FrontTexture;
	IGraphics::CTextureHandle m_TeleTexture;
	IGraphics::CTextureHandle m_SpeedupTexture;
	IGraphics::CTextureHandle m_SwitchTexture;
	IGraphics::CTextureHandle m_TuneTexture;

	int GetTextureUsageFlag() const;

	enum EPreviewState
	{
		PREVIEW_UNLOADED,
		PREVIEW_LOADED,
		PREVIEW_ERROR,
	};

	std::shared_ptr<CLayerGroup> m_apSavedBrushes[10];
	static constexpr ColorRGBA ms_DefaultPropColor = ColorRGBA(1, 1, 1, 0.5f);

public:
	class IInput *Input() const { return m_pInput; }
	class IClient *Client() const { return m_pClient; }
	class IConfigManager *ConfigManager() const { return m_pConfigManager; }
	class CConfig *Config() const { return m_pConfig; }
	class IEngine *Engine() const { return m_pEngine; }
	class IGraphics *Graphics() const { return m_pGraphics; }
	class ISound *Sound() const { return m_pSound; }
	class ITextRender *TextRender() const { return m_pTextRender; }
	class IStorage *Storage() const { return m_pStorage; }
	CUi *Ui() { return &m_UI; }
	CRenderMap *RenderMap() { return &m_RenderMap; }

	CEditorMap *Map() { return &m_Map; }
	const CEditorMap *Map() const { return &m_Map; }
	CMapView *MapView() { return &m_MapView; }
	const CMapView *MapView() const { return &m_MapView; }
	CLayerSelector *LayerSelector() { return &m_LayerSelector; }

	void FillGameTiles(EGameTileOp FillTile) const;
	bool CanFillGameTiles() const;
	void AddQuadOrSound();
	void AddGroup();
	void AddSoundLayer();
	void AddTileLayer();
	void AddQuadsLayer();
	void AddSwitchLayer();
	void AddFrontLayer();
	void AddTuneLayer();
	void AddSpeedupLayer();
	void AddTeleLayer();
	void DeleteSelectedLayer();
	void LayerSelectImage();
	bool IsNonGameTileLayerSelected() const;
	void MapDetails();
	void TestMapLocally();
#define REGISTER_QUICK_ACTION(name, text, callback, disabled, active, button_color, description) CQuickAction m_QuickAction##name;
#include <game/editor/quick_actions.h>
#undef REGISTER_QUICK_ACTION

	CEditor() :
#define REGISTER_QUICK_ACTION(name, text, callback, disabled, active, button_color, description) m_QuickAction##name(text, description, callback, disabled, active, button_color),
#include <game/editor/quick_actions.h>
#undef REGISTER_QUICK_ACTION
		m_ZoomEnvelopeX(1.0f, 0.1f, 600.0f),
		m_ZoomEnvelopeY(640.0f, 0.1f, 32000.0f),
		m_MapSettingsCommandContext(m_MapSettingsBackend.NewContext(&m_SettingsCommandInput)),
		m_Map(this)
	{
		m_EntitiesTexture.Invalidate();
		m_FrontTexture.Invalidate();
		m_TeleTexture.Invalidate();
		m_SpeedupTexture.Invalidate();
		m_SwitchTexture.Invalidate();
		m_TuneTexture.Invalidate();

		m_Mode = MODE_LAYERS;
		m_Dialog = 0;

		m_BrushColorEnabled = true;

		m_aFilenamePendingLoad[0] = '\0';

		m_PopupEventActivated = false;
		m_PopupEventWasActivated = false;

		m_ToolbarPreviewSound = -1;

		m_SelectEntitiesImage = "DDNet";

		m_ResetZoomEnvelope = true;
		m_OffsetEnvelopeX = 0.1f;
		m_OffsetEnvelopeY = 0.5f;

		m_ShowMousePointer = true;

		m_GuiActive = true;
		m_PreviewZoom = false;

		m_ShowTileInfo = SHOW_TILE_OFF;
		m_ShowDetail = true;
		m_Animate = false;
		m_AnimateStart = 0;
		m_AnimateTime = 0;
		m_AnimateSpeed = 1;
		m_AnimateUpdatePopup = false;

		for(size_t i = 0; i < std::size(m_aSavedColors); ++i)
		{
			m_aSavedColors[i] = color_cast<ColorRGBA>(ColorHSLA(i / (float)std::size(m_aSavedColors), 1.0f, 0.5f));
		}

		m_CheckerTexture.Invalidate();
		for(auto &CursorTexture : m_aCursorTextures)
			CursorTexture.Invalidate();

		m_CursorType = CURSOR_NORMAL;

		// DDRace

		m_TeleNumber = 1;
		m_TeleCheckpointNumber = 1;
		m_ViewTeleNumber = 0;

		m_TuningNumber = 1;
		m_ViewTuning = 0;

		m_SwitchNumber = 1;
		m_SwitchDelay = 0;
		m_SpeedupForce = 50;
		m_SpeedupMaxSpeed = 0;
		m_SpeedupAngle = 0;
		m_LargeLayerWasWarned = false;
		m_PreventUnusedTilesWasWarned = false;
		m_AllowPlaceUnusedTiles = EUnusedEntities::NOT_ALLOWED;
		m_BrushDrawDestructive = true;
		m_FillTool = false;
		m_FillShiftX = 0;
		m_FillShiftY = 0;
		m_ShapeTool = false;
		m_ShapeKind = SHAPE_RECT;
		m_ShapeFilled = false;
		m_ShapeThickness = 1;
		m_ShapeNgonSides = 6;
		m_LineTool = false;
		m_LineThickness = 1;
		m_LineActiveNode = -1;
		m_LineHoverNode = -1;
		m_LineClosed = false;
		m_LineDragStartMouse = vec2(0.0f, 0.0f);
		m_LineDragMoved = false;
		m_LineClickedExisting = false;
		m_LineDefaultNodeKind = LINE_NODE_SMOOTH;
		m_SymmetryMode = SYM_NONE;
		m_SymmetryCenter = vec2(0.0f, 0.0f);
		m_SymmetryCenterInit = false;
		m_RecentPickerWasOpen = false;
		m_FadeTool = false;
		m_FadeInit = false;
		m_FadeWidth = 8;
		m_FadeWidthEnd = 8;
		m_FadeFeather = 0;
		m_FadeDensityStart = 0.0f;
		m_FadeDensityEnd = 1.0f;
		m_FadeSeed = 12345;
		m_FadeAirOnUnfilled = false;
		m_FadeActiveHandle = -1;
		m_FadeHoverHandle = -1;
		m_FadeDragStartMouse = vec2(0.0f, 0.0f);
		m_FadeDragStartNodePos = vec2(0.0f, 0.0f);
	}

	class CHoverTile
	{
	public:
		CHoverTile(int Group, int Layer, int x, int y, const CTile Tile) :
			m_Group(Group),
			m_Layer(Layer),
			m_X(x),
			m_Y(y),
			m_Tile(Tile)
		{
		}

		int m_Group;
		int m_Layer;
		int m_X;
		int m_Y;
		const CTile m_Tile;
	};
	std::vector<CHoverTile> m_vHoverTiles;
	const std::vector<CHoverTile> &HoverTiles() const { return m_vHoverTiles; }

	void Init() override;
	void OnUpdate() override;
	void OnRender() override;
	void OnActivate() override;
	void OnWindowResize() override;
	void OnClose() override;
	void OnDialogClose();
	bool HasUnsavedData() const override { return Map()->m_Modified; }
	void UpdateMentions() override { m_Mentions++; }
	void ResetMentions() override { m_Mentions = 0; }
	void OnIngameMoved() override { m_IngameMoved = true; }
	void ResetIngameMoved() override { m_IngameMoved = false; }

	void HandleCursorMovement();
	void OnMouseMove(vec2 MousePos);
	void MouseAxisLock(vec2 &CursorRel);
	vec2 m_MouseAxisInitialPos = vec2(0.0f, 0.0f);
	enum class EAxisLock
	{
		START,
		NONE,
		HORIZONTAL,
		VERTICAL,
	} m_MouseAxisLockState = EAxisLock::START;

	/**
	 * Global time when the autosave was last updated in the @link HandleAutosave @endlink function.
	 * This is used so that the autosave does not immediately activate when reopening the editor after
	 * a longer time of inactivity, as autosaves are only updated while the editor is open.
	 */
	float m_LastAutosaveUpdateTime = -1.0f;
	void HandleAutosave();
	void HandleWriterFinishJobs();

	// TODO: The name of the ShowFileDialogError function is not accurate anymore, this is used for generic error messages.
	//       Popups in UI should be shared_ptrs to make this even more generic.
	class CStringKeyComparator
	{
	public:
		bool operator()(const char *pLhs, const char *pRhs) const;
	};
	std::map<const char *, CUi::SMessagePopupContext *, CStringKeyComparator> m_PopupMessageContexts;
	[[gnu::format(printf, 2, 3)]] void ShowFileDialogError(const char *pFormat, ...);

	void Reset(bool CreateDefault = true);
	bool Save(const char *pFilename) override;
	bool Load(const char *pFilename, int StorageType) override;
	bool HandleMapDrop(const char *pFilename, int StorageType) override;
	void LoadCurrentMap();
	void Render();

	void RenderPressedKeys(CUIRect View);
	void RenderSavingIndicator(CUIRect View);
	void FreeDynamicPopupMenus();
	void UpdateColorPipette();
	void RenderMousePointer();
	void RenderGameEntities(const std::shared_ptr<CLayerTiles> &pTiles);
	void RenderSwitchEntities(const std::shared_ptr<CLayerTiles> &pTiles);

	template<typename E>
	SEditResult<E> DoPropertiesWithState(CUIRect *pToolbox, CProperty *pProps, int *pIds, int *pNewVal, const std::vector<ColorRGBA> &vColors = {});
	int DoProperties(CUIRect *pToolbox, CProperty *pProps, int *pIds, int *pNewVal, const std::vector<ColorRGBA> &vColors = {});

	CUi::SColorPickerPopupContext m_ColorPickerPopupContext;
	const void *m_pColorPickerPopupActiveId = nullptr;
	void DoColorPickerButton(const void *pId, const CUIRect *pRect, ColorRGBA Color, const std::function<void(ColorRGBA Color)> &SetColor);

	int m_Mode;
	int m_Dialog;
	char m_aTooltip[256] = "";

	bool m_BrushColorEnabled;

	/**
	 * File which is pending to be loaded by @link POPEVENT_LOADDROP @endlink.
	 */
	char m_aFilenamePendingLoad[IO_MAX_PATH_LENGTH] = "";

	enum
	{
		POPEVENT_EXIT = 0,
		POPEVENT_LOAD,
		POPEVENT_LOADCURRENT,
		POPEVENT_LOADDROP,
		POPEVENT_NEW,
		POPEVENT_LARGELAYER,
		POPEVENT_PREVENTUNUSEDTILES,
		POPEVENT_IMAGEDIV16,
		POPEVENT_IMAGE_MAX,
		POPEVENT_SOUND_MAX,
		POPEVENT_PLACE_BORDER_TILES,
		POPEVENT_TILE_ART_BIG_IMAGE,
		POPEVENT_TILE_ART_MANY_COLORS,
		POPEVENT_TILE_ART_TOO_MANY_COLORS,
		POPEVENT_QUAD_ART_BIG_IMAGE,
		POPEVENT_REMOVE_USED_IMAGE,
		POPEVENT_REMOVE_USED_SOUND,
		POPEVENT_RESTART_SERVER,
		POPEVENT_RESTARTING_SERVER,
	};

	int m_PopupEventType;
	int m_PopupEventActivated;
	int m_PopupEventWasActivated;
	bool m_LargeLayerWasWarned;
	bool m_PreventUnusedTilesWasWarned;

	enum class EUnusedEntities
	{
		ALLOWED_IMPLICIT = -1,
		NOT_ALLOWED = 0,
		ALLOWED_EXPLICIT = 1,
	};
	EUnusedEntities m_AllowPlaceUnusedTiles;
	bool IsAllowPlaceUnusedTiles() const;

	bool m_BrushDrawDestructive;
	bool m_FillTool;
	int m_FillShiftX;
	int m_FillShiftY;

	enum EShapeKind
	{
		SHAPE_RECT = 0,
		SHAPE_ELLIPSE = 1,
		SHAPE_TRIANGLE = 2,
		SHAPE_NGON = 3,
	};
	bool m_ShapeTool;
	int m_ShapeKind;
	bool m_ShapeFilled;
	int m_ShapeThickness;
	int m_ShapeNgonSides;
	// Persistent ghost for shape tool (nodes + Enter to stamp)
	bool m_ShapeGhostInit = false;
	int m_ShapeGhostX1 = 0, m_ShapeGhostY1 = 0, m_ShapeGhostX2 = 9, m_ShapeGhostY2 = 9;
	int m_ShapeGhostDragNode = -1;
	int m_ShapeGhostHoverNode = -1;
	vec2 m_ShapeGhostDragStartMouse = {};
	int m_ShapeGhostDragStartX1 = 0, m_ShapeGhostDragStartY1 = 0;
	int m_ShapeGhostDragStartX2 = 0, m_ShapeGhostDragStartY2 = 0;
	void ShapeGhostClear();
	void ShapeGhostCommit();
	int ShapeGhostHitNode(float wx, float wy) const;
	void ShapeGhostRenderAndInteract(float wx, float wy, float mx, float my);

	bool m_LineTool;
	int m_LineThickness;
	std::vector<vec2> m_vLineNodes;
	std::vector<vec2> m_vLineHandleIn;
	std::vector<vec2> m_vLineHandleOut;
	int m_LineActiveNode;
	int m_LineHoverNode;
	int m_LineSelectedNode = -1; // node whose bezier handles are visible/interactive
	enum ELineDragKind
	{
		LINE_DRAG_NONE = 0,
		LINE_DRAG_NODE,
		LINE_DRAG_HANDLE_IN,
		LINE_DRAG_HANDLE_OUT,
	};
	int m_LineDragKind = LINE_DRAG_NONE;
	int m_LineHoverDragKind = LINE_DRAG_NONE;
	vec2 m_LineDragStartHandleIn;
	vec2 m_LineDragStartHandleOut;
	bool m_LineClosed;
	vec2 m_LineDragStartMouse;
	bool m_LineDragMoved;
	bool m_LineClickedExisting;
	enum ELineNodeKind
	{
		LINE_NODE_SMOOTH = 0,
		LINE_NODE_SHARP = 1,
	};
	int m_LineDefaultNodeKind;
	bool m_LineCatmullRom = false;
	std::vector<int> m_vLineNodeKinds;

	enum ESymmetryMode
	{
		SYM_NONE = 0,
		SYM_H = 1,
		SYM_V = 2,
		SYM_BOTH = 3,
	};
	int m_SymmetryMode;
	vec2 m_SymmetryCenter;
	bool m_SymmetryCenterInit;

	struct CRecentTile
	{
		CTile m_Tile{};
		int m_ImageId = -1;
		bool m_HasGame = false;
		bool m_HasFront = false;
		bool m_HasTele = false;
		bool m_HasSwitch = false;
		bool m_HasSpeedup = false;
		bool m_HasTune = false;
	};
	std::vector<CRecentTile> m_vRecentTiles;
	bool m_RecentPickerWasOpen;
	void RecentTilesSnapshot();
	void RecentTilesUse(int Index);

	bool m_FadeTool;
	bool m_FadeInit;
	std::vector<vec2> m_vFadeNodes; // node positions
	std::vector<vec2> m_vFadeHandleIn; // offset from node to incoming-segment control point
	std::vector<vec2> m_vFadeHandleOut; // offset from node to outgoing-segment control point
	int m_FadeWidth; // half-width at the start of the path, in tiles
	int m_FadeWidthEnd; // half-width at the end of the path, in tiles (linearly interpolated)
	int m_FadeFeather; // additional tiles of soft falloff beyond width
	float m_FadeDensityStart;
	float m_FadeDensityEnd;
	int m_FadeSeed;
	bool m_FadeAirOnUnfilled;
	enum EFadeDragKind
	{
		FADE_DRAG_NONE = 0,
		FADE_DRAG_NODE,
		FADE_DRAG_HANDLE_IN,
		FADE_DRAG_HANDLE_OUT,
	};
	int m_FadeDragKind = FADE_DRAG_NONE;
	int m_FadeActiveHandle; // node index when kind is NODE/HANDLE_IN/HANDLE_OUT, else -1
	int m_FadeHoverHandle;
	int m_FadeHoverDragKind = FADE_DRAG_NONE;
	vec2 m_FadeDragStartMouse;
	vec2 m_FadeDragStartNodePos;
	vec2 m_FadeDragStartHandleIn;
	vec2 m_FadeDragStartHandleOut;
	void FadeToolEnsureInit();
	void FadeToolCancel();
	void FadeToolCommit();
	void FadeToolRender(float wx, float wy);
	int FadeToolHitTest(float wx, float wy) const;
	int FadeToolHandleHitTest(float wx, float wy, int &OutKind) const;
	int FadeToolFindHoveredSegment(float wx, float wy, vec2 &OutClosest) const;
	void FadeToolApplyDrag(float wx, float wy);
	bool FadeToolIsCellFilled(int CellX, int CellY) const;
	float FadeToolDensityAt(int CellX, int CellY) const;
	void FadeToolGetClosest(vec2 P, float &OutT, float &OutDist) const;
	void FadeRecomputeAutoHandles(int NodeIndex);
	static vec2 FadeToolCubicBezierAt(vec2 P0, vec2 H0, vec2 H1, vec2 P1, float t);
	static vec2 FadeToolBezierAt(vec2 P0, vec2 P1, vec2 P2, float t);
	static float FadeToolHash01(int x, int y, int Seed);
	void LineToolCommit(bool Closed = false);
	void LineToolCancel();
	void LineToolRenderAndInteract(float wx, float wy);
	void LineToolSamplePath(std::vector<vec2> &OutSamples, bool Closed = false) const;
	void LineToolCollectCells(std::vector<std::pair<int, int>> &OutCells, bool Closed = false) const;
	int LineToolFindHoveredNode(float wx, float wy) const;
	int LineToolFindHoveredSegment(float wx, float wy, vec2 &OutClosest) const;
	int LineToolHandleHitTest(float wx, float wy, int &OutKind) const;
	void LineRecomputeAutoHandles(int NodeIndex);
	static vec2 CatmullRom(vec2 P0, vec2 P1, vec2 P2, vec2 P3, float t);

	bool m_ThreeDTool = false;
	bool m_ThreeDInit = false;
	enum EThreeDShape
	{
		THREED_CUBE = 0,
		THREED_SPHERE = 1,
		THREED_CYLINDER = 2,
		THREED_TORUS = 3,
	};
	enum EThreeDRenderMode
	{
		THREED_RENDER_GHOST = 0,
		THREED_RENDER_WIRE = 1,
		THREED_RENDER_BOTH = 2,
	};
	int m_ThreeDShape = THREED_CUBE;
	int m_ThreeDRenderMode = THREED_RENDER_GHOST;
	int m_ThreeDGridDiv = 4;
	vec2 m_ThreeDCenter = {};
	float m_ThreeDScaleX = 3.0f * 32.0f;
	float m_ThreeDScaleY = 3.0f * 32.0f;
	float m_ThreeDScaleZ = 3.0f * 32.0f;
	float m_ThreeDRxDeg = 20.0f;
	float m_ThreeDRyDeg = 30.0f;
	float m_ThreeDRzDeg = 0.0f;
	float m_ThreeDPerspTiles = 20.0f;
	int m_ThreeDDragNode = -1;
	int m_ThreeDHoverNode = -1;
	vec2 m_ThreeDDragStartMouse = {};
	vec2 m_ThreeDDragStartCenter = {};
	float m_ThreeDDragStartScaleX = 0.0f;
	float m_ThreeDDragStartScaleY = 0.0f;
	float m_ThreeDDragStartScaleZ = 0.0f;
	float m_ThreeDDragStartRx = 0.0f;
	float m_ThreeDDragStartRy = 0.0f;
	void ThreeDToolEnsureInit();
	void ThreeDToolCancel();
	void ThreeDToolCommit();
	int ThreeDToolHitNode(float wx, float wy) const;
	void ThreeDToolRenderAndInteract(float wx, float wy, float mx, float my);
	static vec3 ThreeDRotate(vec3 p, float rx, float ry, float rz);
	static vec2 ThreeDProject(vec3 p, float perspPx);
	static void ThreeDCollectCells(int Shape, float Rx, float Ry, float Rz, float Sx, float Sy, float Sz, float PerspPx, float Cx, float Cy, int GridDiv, std::set<std::pair<int, int>> &Out);

	bool m_SelectTool = false;
	bool m_SelectFreehand = false;
	std::vector<vec2> m_vSelectPath;
	std::vector<CTile> m_vSelectOrigTiles;
	int m_SelectOrigW = 0;
	int m_SelectOrigH = 0;
	float m_SelectTotalAngle = 0.0f;
	void SelectToolSaveOrig();
	void SelectToolCancelOrig();

	int m_Mentions = 0;
	bool m_IngameMoved = false;

	int m_ToolbarPreviewSound;

	std::vector<std::string> m_vSelectEntitiesFiles;
	std::string m_SelectEntitiesImage;

	// Zooming
	CSmoothValue m_ZoomEnvelopeX;
	CSmoothValue m_ZoomEnvelopeY;

	bool m_ResetZoomEnvelope;

	float m_OffsetEnvelopeX;
	float m_OffsetEnvelopeY;

	bool m_ShowMousePointer;
	bool m_GuiActive;

	bool m_PreviewZoom;
	float m_MouseWorldScale = 1.0f; // Mouse (i.e. UI) scale relative to the World (selected Group)
	vec2 m_MouseWorldPos = vec2(0.0f, 0.0f);
	vec2 m_MouseWorldNoParaPos = vec2(0.0f, 0.0f);
	vec2 m_MouseDeltaWorld = vec2(0.0f, 0.0f);
	const void *m_pContainerPanned;
	const void *m_pContainerPannedLast;
	char m_MapEditorId; // UI element ID for the main map editor

	enum EShowTile
	{
		SHOW_TILE_OFF,
		SHOW_TILE_DECIMAL,
		SHOW_TILE_HEXADECIMAL
	};
	EShowTile m_ShowTileInfo;
	bool m_ShowDetail;

	bool m_Animate;
	int64_t m_AnimateStart;
	float m_AnimateTime;
	float m_AnimateSpeed;
	bool m_AnimateUpdatePopup;

	enum EExtraEditor
	{
		EXTRAEDITOR_NONE = -1,
		EXTRAEDITOR_ENVELOPES,
		EXTRAEDITOR_SERVER_SETTINGS,
		EXTRAEDITOR_HISTORY,
		NUM_EXTRAEDITORS,
	};
	EExtraEditor m_ActiveExtraEditor = EXTRAEDITOR_NONE;
	float m_aExtraEditorSplits[NUM_EXTRAEDITORS] = {250.0f, 250.0f, 250.0f};
	float m_ToolBoxWidth = 100.0f;

	bool m_ShowEnvelopePreview = false;
	enum class EEnvelopePreview
	{
		NONE,
		SELECTED,
		ALL,
	};
	EEnvelopePreview m_ActiveEnvelopePreview = EEnvelopePreview::NONE;
	enum class EQuadEnvelopePointOperation
	{
		NONE = 0,
		MOVE,
		ROTATE,
	};
	EQuadEnvelopePointOperation m_QuadEnvelopePointOperation = EQuadEnvelopePointOperation::NONE;

	bool m_ShowPicker;

	// Color palette and pipette
	ColorRGBA m_aSavedColors[8];
	ColorRGBA m_PipetteColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	bool m_ColorPipetteActive = false;

	IGraphics::CTextureHandle m_CheckerTexture;

	enum ECursorType
	{
		CURSOR_NORMAL,
		CURSOR_RESIZE_V,
		CURSOR_RESIZE_H,
		NUM_CURSORS
	};
	IGraphics::CTextureHandle m_aCursorTextures[ECursorType::NUM_CURSORS];
	ECursorType m_CursorType;

	IGraphics::CTextureHandle GetEntitiesTexture();

	std::shared_ptr<CLayerGroup> m_pBrush;
	std::shared_ptr<CLayerTiles> m_pTilesetPicker;
	std::shared_ptr<CLayerQuads> m_pQuadsetPicker;

	const void *m_pUiGotContext = nullptr;

	std::deque<std::shared_ptr<CDataFileWriterFinishJob>> m_WriterFinishJobs;

	void EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) override;

	CLineInputBuffered<256> m_SettingsCommandInput;
	CMapSettingsBackend m_MapSettingsBackend;
	CMapSettingsBackend::CContext m_MapSettingsCommandContext;

	CImageInfo m_TileArtImageInfo;
	void AddTileArt(bool IgnoreHistory = false);
	char m_aTileArtFilename[IO_MAX_PATH_LENGTH];
	void TileArtCheckColors();

	CImageInfo m_QuadArtImageInfo;
	CQuadArtParameters m_QuadArtParameters;
	void AddQuadArt(bool IgnoreHistory = false);

	// editor_ui.cpp
	void UpdateTooltip(const void *pId, const CUIRect *pRect, const char *pToolTip);
	ColorRGBA GetButtonColor(const void *pId, int Checked);
	int DoButtonLogic(const void *pId, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip);
	int DoButton_Editor(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip);
	int DoButton_Env(const void *pId, const char *pText, int Checked, const CUIRect *pRect, const char *pToolTip, ColorRGBA Color, int Corners);
	int DoButton_Ex(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip, int Corners, float FontSize = EditorFontSizes::MENU, int Align = TEXTALIGN_MC);
	int DoButton_FontIcon(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip, int Corners, float FontSize = 10.0f);
	int DoButton_MenuItem(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags = BUTTONFLAG_LEFT, const char *pToolTip = nullptr);
	int DoButton_DraggableEx(const void *pId, const char *pText, int Checked, const CUIRect *pRect, bool *pClicked, bool *pAbrupted, int Flags, const char *pToolTip = nullptr, int Corners = IGraphics::CORNER_ALL, float FontSize = 10.0f);
	bool DoEditBox(CLineInput *pLineInput, const CUIRect *pRect, float FontSize, int Corners = IGraphics::CORNER_ALL, const char *pToolTip = nullptr, const std::vector<STextColorSplit> &vColorSplits = {});
	bool DoClearableEditBox(CLineInput *pLineInput, const CUIRect *pRect, float FontSize, int Corners = IGraphics::CORNER_ALL, const char *pToolTip = nullptr, const std::vector<STextColorSplit> &vColorSplits = {});
	SEditResult<int> UiDoValueSelector(const void *pId, CUIRect *pRect, const char *pLabel, int Current, int Min, int Max, int Step, float Scale, const char *pToolTip, bool IsDegree = false, bool IsHex = false, int Corners = IGraphics::CORNER_ALL, const ColorRGBA *pColor = nullptr, bool ShowValue = true);
	void RenderBackground(CUIRect View, IGraphics::CTextureHandle Texture, float Size, float Brightness) const;

	// editor_server_settings.cpp
	void DoMapSettingsEditBox(CMapSettingsBackend::CContext *pContext, const CUIRect *pRect, float FontSize, float DropdownMaxHeight, int Corners = IGraphics::CORNER_ALL, const char *pToolTip = nullptr);
	template<typename T>
	int DoEditBoxDropdown(SEditBoxDropdownContext *pDropdown, CLineInput *pLineInput, const CUIRect *pEditBoxRect, int x, float MaxHeight, bool AutoWidth, const std::vector<T> &vData, const FDropdownRenderCallback<T> &pfnMatchCallback);
	template<typename T>
	int RenderEditBoxDropdown(SEditBoxDropdownContext *pDropdown, CUIRect View, CLineInput *pLineInput, int x, float MaxHeight, bool AutoWidth, const std::vector<T> &vData, const FDropdownRenderCallback<T> &pfnMatchCallback);

	static CUi::EPopupMenuFunctionResult PopupMenuFile(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupMenuTools(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupMenuSettings(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupGroup(void *pContext, CUIRect View, bool Active);
	struct SLayerPopupContext : public SPopupMenuId
	{
		CEditor *m_pEditor;
		std::vector<std::shared_ptr<CLayerTiles>> m_vpLayers;
		std::vector<int> m_vLayerIndices;
		CLayerTiles::SCommonPropState m_CommonPropState;
	};
	static CUi::EPopupMenuFunctionResult PopupLayer(void *pContext, CUIRect View, bool Active);
	class CQuadPopupContext : public SPopupMenuId
	{
	public:
		CEditor *m_pEditor;
		int m_SelectedQuadIndex;
		int m_Color;
	};
	CQuadPopupContext m_QuadPopupContext;
	static CUi::EPopupMenuFunctionResult PopupQuad(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSource(void *pContext, CUIRect View, bool Active);
	class CPointPopupContext : public SPopupMenuId
	{
	public:
		CEditor *m_pEditor;
		int m_SelectedQuadPoint;
		int m_SelectedQuadIndex;
	};
	CPointPopupContext m_PointPopupContext;
	static CUi::EPopupMenuFunctionResult PopupPoint(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupEnvPoint(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupEnvPointMulti(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupEnvPointCurveType(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupImage(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSound(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupMapInfo(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupEvent(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSelectImage(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSelectSound(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSelectGametileOp(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSelectConfigAutoMap(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSelectAutoMapReference(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupTele(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSpeedup(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupSwitch(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupTune(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupGoto(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupEntities(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupProofMode(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupAnimateSettings(void *pContext, CUIRect View, bool Active);
	int m_PopupEnvelopeSelectedPoint = -1;
	static CUi::EPopupMenuFunctionResult PopupEnvelopeCurvetype(void *pContext, CUIRect View, bool Active);
	static CUi::EPopupMenuFunctionResult PopupQuadArt(void *pContext, CUIRect View, bool Active);

	static bool CallbackOpenMap(const char *pFilename, int StorageType, void *pUser);
	static bool CallbackAppendMap(const char *pFilename, int StorageType, void *pUser);
	static bool CallbackSaveMap(const char *pFilename, int StorageType, void *pUser);
	static bool CallbackSaveCopyMap(const char *pFilename, int StorageType, void *pUser);
	static bool CallbackAddTileArt(const char *pFilepath, int StorageType, void *pUser);
	static bool CallbackAddQuadArt(const char *pFilepath, int StorageType, void *pUser);
	static bool CallbackSaveImage(const char *pFilename, int StorageType, void *pUser);
	static bool CallbackSaveSound(const char *pFilename, int StorageType, void *pUser);
	static bool CallbackCustomEntities(const char *pFilename, int StorageType, void *pUser);

	void PopupSelectImageInvoke(int Current, float x, float y);
	int PopupSelectImageResult();

	void PopupSelectGametileOpInvoke(float x, float y);
	int PopupSelectGameTileOpResult();

	void PopupSelectConfigAutoMapInvoke(int Current, float x, float y);
	int PopupSelectConfigAutoMapResult();

	void PopupSelectSoundInvoke(int Current, float x, float y);
	int PopupSelectSoundResult();

	void PopupSelectAutoMapReferenceInvoke(int Current, float x, float y);
	int PopupSelectAutoMapReferenceResult();

	void DoQuadEnvelopes(const CLayerQuads *pLayerQuads);
	void DoQuadEnvPoint(const CQuad *pQuad, CEnvelope *pEnvelope, int QuadIndex, int PointIndex);
	void DoQuadPoint(int LayerIndex, const std::shared_ptr<CLayerQuads> &pLayer, CQuad *pQuad, int QuadIndex, int v);
	void UpdateHotQuadPoint(const CLayerQuads *pLayer);

	float TriangleArea(vec2 A, vec2 B, vec2 C);
	bool IsInTriangle(vec2 Point, vec2 A, vec2 B, vec2 C);
	void DoQuadKnife(int QuadIndex);

	void DoSoundSource(int LayerIndex, CSoundSource *pSource, int Index);
	void UpdateHotSoundSource(const CLayerSounds *pLayer);

	enum class EAxis
	{
		NONE = 0,
		X,
		Y,
	};
	struct SAxisAlignedBoundingBox
	{
		enum
		{
			POINT_TL = 0,
			POINT_TR,
			POINT_BL,
			POINT_BR,
			POINT_CENTER,
			NUM_POINTS
		};
		CPoint m_aPoints[NUM_POINTS];
	};
	void DoMapEditor(CUIRect View);
	void DoToolbarLayers(CUIRect Toolbar);
	void DoToolbarImages(CUIRect Toolbar);
	void DoToolbarSounds(CUIRect Toolbar);
	void DoQuad(int LayerIndex, const std::shared_ptr<CLayerQuads> &pLayer, CQuad *pQuad, int Index);
	void PreparePointDrag(const CQuad *pQuad, int QuadIndex, int PointIndex);
	void DoPointDrag(CQuad *pQuad, int QuadIndex, int PointIndex, ivec2 Offset);
	EAxis GetDragAxis(ivec2 Offset) const;
	void DrawAxis(EAxis Axis, CPoint &OriginalPoint, CPoint &Point) const;
	void DrawAABB(const SAxisAlignedBoundingBox &AABB, ivec2 Offset) const;

	// Alignment methods
	// These methods take `OffsetX` and `OffsetY` because the calculations are made with the original positions
	// of the quad(s), before we started dragging. This allows us to edit `OffsetX` and `OffsetY` based on the previously
	// calculated alignments.
	struct SAlignmentInfo
	{
		CPoint m_AlignedPoint; // The "aligned" point, which we want to align/snap to
		union
		{
			// The current changing value when aligned to this point. When aligning to a point on the X axis, then the X value is changing because
			// we aligned the Y values (X axis aligned => Y values are the same, Y axis aligned => X values are the same).
			int m_X;
			int m_Y;
		};
		EAxis m_Axis; // The axis we are aligning on
		int m_PointIndex; // The point index we are aligning
		int m_Diff; // Store the difference
	};
	void ComputePointAlignments(const std::shared_ptr<CLayerQuads> &pLayer, CQuad *pQuad, int QuadIndex, int PointIndex, ivec2 Offset, std::vector<SAlignmentInfo> &vAlignments, bool Append = false) const;
	void ComputePointsAlignments(const std::shared_ptr<CLayerQuads> &pLayer, bool Pivot, ivec2 Offset, std::vector<SAlignmentInfo> &vAlignments) const;
	void ComputeAABBAlignments(const std::shared_ptr<CLayerQuads> &pLayer, const SAxisAlignedBoundingBox &AABB, ivec2 Offset, std::vector<SAlignmentInfo> &vAlignments) const;
	void DrawPointAlignments(const std::vector<SAlignmentInfo> &vAlignments, ivec2 Offset) const;
	void QuadSelectionAABB(const std::shared_ptr<CLayerQuads> &pLayer, SAxisAlignedBoundingBox &OutAABB);
	void ApplyAlignments(const std::vector<SAlignmentInfo> &vAlignments, ivec2 &Offset);
	void ApplyAxisAlignment(ivec2 &Offset) const;

	bool ReplaceImage(const char *pFilename, int StorageType, bool CheckDuplicate);
	static bool ReplaceImageCallback(const char *pFilename, int StorageType, void *pUser);
	bool ReplaceSound(const char *pFilename, int StorageType, bool CheckDuplicate);
	static bool ReplaceSoundCallback(const char *pFilename, int StorageType, void *pUser);
	static bool AddImage(const char *pFilename, int StorageType, void *pUser);
	static bool AddSound(const char *pFilename, int StorageType, void *pUser);

	static bool IsVanillaImage(const char *pImage);

	void RenderLayers(CUIRect LayersBox);
	void RenderImagesList(CUIRect Toolbox);
	void RenderSelectedImage(CUIRect View) const;
	void RenderSounds(CUIRect Toolbox);
	void RenderModebar(CUIRect View);
	void RenderStatusbar(CUIRect View, CUIRect *pTooltipRect);
	void RenderTooltip(CUIRect TooltipRect);

	void RenderEnvelopeEditor(CUIRect View);
	void RenderEnvelopeEditorColorBar(CUIRect ColorBar, const std::shared_ptr<CEnvelope> &pEnvelope);

	void RenderMapSettingsErrorDialog();
	void RenderServerSettingsEditor(CUIRect View, bool ShowServerSettingsEditorLast);
	static void MapSettingsDropdownRenderCallback(const SPossibleValueMatch &Match, char (&aOutput)[128], std::vector<STextColorSplit> &vColorSplits);

	void RenderEditorHistory(CUIRect View);

	enum class EDragSide // Which side is the drag bar on
	{
		BOTTOM,
		LEFT,
		TOP,
		RIGHT,
	};
	void DoEditorDragBar(CUIRect View, CUIRect *pDragBar, EDragSide Side, float *pValue, float MinValue = 100.0f, float MaxValue = 400.0f);

	void UpdateHotEnvelopePoint(const CUIRect &View, const CEnvelope *pEnvelope, int ActiveChannels);

	void RenderMenubar(CUIRect Menubar);
	void ShowHelp();

	void DoAudioPreview(CUIRect View, const void *pPlayPauseButtonId, const void *pStopButtonId, const void *pSeekBarId, int SampleId);

	// Zooming
	void ZoomAdaptOffsetX(float ZoomFactor, const CUIRect &View);
	void UpdateZoomEnvelopeX(const CUIRect &View);

	void ZoomAdaptOffsetY(float ZoomFactor, const CUIRect &View);
	void UpdateZoomEnvelopeY(const CUIRect &View);

	void ResetZoomEnvelope(const std::shared_ptr<CEnvelope> &pEnvelope, int ActiveChannels);
	void RemoveTimeOffsetEnvelope(const std::shared_ptr<CEnvelope> &pEnvelope);
	float ScreenToEnvelopeX(const CUIRect &View, float x) const;
	float EnvelopeToScreenX(const CUIRect &View, float x) const;
	float ScreenToEnvelopeY(const CUIRect &View, float y) const;
	float EnvelopeToScreenY(const CUIRect &View, float y) const;
	float ScreenToEnvelopeDX(const CUIRect &View, float DeltaX);
	float ScreenToEnvelopeDY(const CUIRect &View, float DeltaY);

	// DDRace

	IGraphics::CTextureHandle GetFrontTexture();
	IGraphics::CTextureHandle GetTeleTexture();
	IGraphics::CTextureHandle GetSpeedupTexture();
	IGraphics::CTextureHandle GetSwitchTexture();
	IGraphics::CTextureHandle GetTuneTexture();

	unsigned char m_TeleNumber;
	unsigned char m_TeleCheckpointNumber;
	unsigned char m_ViewTeleNumber;

	unsigned char m_TuningNumber;
	unsigned char m_ViewTuning;

	unsigned char m_SpeedupForce;
	unsigned char m_SpeedupMaxSpeed;
	short m_SpeedupAngle;

	unsigned char m_SwitchNumber;
	unsigned char m_SwitchDelay;
	unsigned char m_ViewSwitch;

	void AdjustBrushSpecialTiles(bool UseNextFree, int Adjust = 0);

private:
	CEditorMap m_Map;

	CEditorHistory &ActiveHistory();

	std::map<int, CPoint[5]> m_QuadDragOriginalPoints;
};

#endif
