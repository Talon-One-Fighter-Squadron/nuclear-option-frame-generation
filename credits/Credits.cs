using System.Collections;
using BepInEx;
using HarmonyLib;
using TMPro;
using UnityEngine;
using UnityEngine.UI;

[BepInPlugin("local.nuclearoption.framegeneration.credits", "Frame Generation Credits", "0.5.0")]
[BepInDependency("local.nuclearoption.dlss")]
public sealed class FrameGenerationCredits : BaseUnityPlugin
{
    const string Technology = "DLSS and Reflex by NVIDIA\nFidelityFX Frame Generation by AMD";
    const string PageName = "Frame generation credits";
    Harmony hooks;
    GameObject startup;

    void Awake()
    {
        hooks = new Harmony("local.nuclearoption.framegeneration.credits.menu");
        hooks.Patch(AccessTools.Method(typeof(GraphicsMenu), "Start"),
            postfix: new HarmonyMethod(typeof(FrameGenerationCredits), nameof(AddCredits)));
    }

    IEnumerator Start()
    {
        // Wait for the first rendered game frames so players can read the startup credit.
        yield return new WaitForEndOfFrame();
        startup = new GameObject("Graphics mod startup credits", typeof(Canvas), typeof(CanvasScaler));
        startup.transform.SetParent(transform, false);
        var canvas = startup.GetComponent<Canvas>();
        canvas.renderMode = RenderMode.ScreenSpaceOverlay;
        canvas.sortingOrder = 30000;
        var scale = startup.GetComponent<CanvasScaler>();
        scale.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
        scale.referenceResolution = new Vector2(1280, 720);
        scale.matchWidthOrHeight = 0.5f;

        var panel = new GameObject("Credit background", typeof(Image));
        panel.transform.SetParent(startup.transform, false);
        var background = panel.GetComponent<Image>();
        background.color = new Color(0.055f, 0.065f, 0.075f, 0.9f);
        background.raycastTarget = false;
        var rect = background.rectTransform;
        rect.anchorMin = rect.anchorMax = rect.pivot = new Vector2(1, 0);
        rect.anchoredPosition = new Vector2(-12, 12);
        rect.sizeDelta = new Vector2(284, 42);

        var label = new GameObject("Technology credits", typeof(Text)).GetComponent<Text>();
        label.transform.SetParent(panel.transform, false);
        label.rectTransform.anchorMin = Vector2.zero;
        label.rectTransform.anchorMax = Vector2.one;
        label.rectTransform.offsetMin = new Vector2(8, 2);
        label.rectTransform.offsetMax = new Vector2(-8, -2);
        label.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
        label.fontSize = 12;
        label.color = Color.white;
        label.alignment = TextAnchor.MiddleLeft;
        label.raycastTarget = false;
        label.text = Technology;
        yield return new WaitForSecondsRealtime(1.75f);
        Destroy(startup);
    }

    static void AddCredits(GraphicsMenu __instance)
    {
        var content = __instance.transform.Find("Scroll View/Viewport/Content");
        if (!content || content.Find(PageName)) return;
        var template = content.GetComponentInChildren<TMP_Text>();
        if (!template) return;

        var block = new GameObject(PageName, typeof(RectTransform), typeof(LayoutElement));
        block.transform.SetParent(content, false);
        var layout = block.GetComponent<LayoutElement>();
        layout.preferredHeight = 112;
        layout.minHeight = 112;
        var text = block.AddComponent<TextMeshProUGUI>();
        text.font = template.font;
        text.fontSize = 20;
        text.color = template.color;
        text.alignment = TextAlignmentOptions.Center;
        text.raycastTarget = false;
        text.text = "GRAPHICS MOD CREDITS\n" + Technology + "\nStreamline by NVIDIA · MinHook by Tsuda Kageyu";
    }

    void OnDestroy()
    {
        if (hooks != null) hooks.UnpatchSelf();
        if (startup) Destroy(startup);
    }
}
