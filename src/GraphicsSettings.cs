using System;
using System.Collections.Generic;
using HarmonyLib;
using TMPro;
using UnityEngine;
using UnityEngine.UI;
using UnityEngine.Events;

public sealed partial class NuclearDLSS
{
    Harmony menuHooks;
    NativeGraphicsSettings nativeMenu;
    void InstallGraphicsMenu() {
        try {
            menuHooks=new Harmony("local.nuclearoption.dlss.graphicsmenu");
            menuHooks.Patch(AccessTools.Method(typeof(GraphicsMenu),"Start"),postfix:HM("AddGraphicsSettings"));
            menuHooks.Patch(AccessTools.Method(typeof(GraphicsMenu),"ResetGraphicsSettings"),postfix:HM("ResetDLSSSettings"));
        } catch(Exception e) {Logger.LogWarning("Graphics menu integration: "+e.GetBaseException().Message);}
    }
    static void ResetDLSSSettings() {
        var p=self;
        if(!p) return;
        p.quality.Value=Quality.DLAA;p.fsrQuality.Value=FsrPreset.Off;p.preferFsrScaling.Value=false;p.nativePanel.Value=true;p.forceSecondGen.Value=false;
        p.fg.Value=false;p.mfg.Value=false;p.dynamic.Value=false;p.multiplier.Value=3;p.fpsOverlay.Value=false;p.reflexBoost.Value=false;
        p.capToRefresh.Value=false;p.provider.Value="";
    }
    static void AddGraphicsSettings(GraphicsMenu __instance,TMP_Dropdown ___aaDropdown,Toggle ___vsyncToggle) {
        if(!self || __instance.GetComponentInChildren<NativeGraphicsSettings>(true)) return;
        GameObject section=null;
        try {
            // Reuse the stock two-column section inside the Graphics scroll view.
            var columns=___aaDropdown.transform.parent.parent.parent;
            section=Instantiate(columns.gameObject,columns.parent,false);
            section.name="Upscaling and Frame Generation";
            section.transform.SetSiblingIndex(columns.GetSiblingIndex());
            var view=section.AddComponent<NativeGraphicsSettings>();
            view.Build(self,___aaDropdown,___vsyncToggle);
            self.nativeMenu=view;
            LayoutRebuilder.ForceRebuildLayoutImmediate((RectTransform)columns.parent);
        } catch(Exception e) {
            if(section) {section.SetActive(false);Destroy(section);}
            self.Logger.LogWarning("Could not build the DLSS controls: "+e.GetBaseException().Message);
        }
    }

    sealed class NativeGraphicsSettings : MonoBehaviour {
        NuclearDLSS mod;
        TMP_Dropdown scaler,preset,model,factor,reflex,provider;
        Toggle gen,multi,instruments,rates;
        Toggle refreshCap;
        TMP_Text refreshLabel;
        TMP_Text srNote,fgNote;
        int previousMax=-1,previousDynamic=-1;
        readonly List<int> factors=new List<int>();

        static Transform PrepareColumn(Transform column,string title) {
            foreach(Transform child in column) {
                var heading=child.GetComponent<TMP_Text>();
                if(heading) { heading.text=title;continue; }
                child.gameObject.SetActive(false);
                Destroy(child.gameObject);
            }
            return column;
        }
        static TMP_Text Note(Transform column) {
            var heading=column.GetComponentInChildren<TMP_Text>();
            var note=Instantiate(heading,column,false);
            note.gameObject.name="Availability";
            note.fontSize=16;note.enableAutoSizing=true;note.fontSizeMin=12;note.fontSizeMax=16;
            note.gameObject.SetActive(false);
            return note;
        }
        static TMP_Dropdown Dropdown(TMP_Dropdown source,Transform column,string label,string[] options,UnityAction<int> changed) {
            var row=Instantiate(source.transform.parent.gameObject,column,false);
            row.name=label;
            var control=row.GetComponentInChildren<TMP_Dropdown>(true);
            // Replacing the serialized event keeps the stock MSAA handler off the copy.
            control.onValueChanged=new TMP_Dropdown.DropdownEvent();
            foreach(Transform child in row.transform) {
                var text=child.GetComponent<TMP_Text>();
                if(text) text.text=label;
            }
            control.ClearOptions();control.AddOptions(new List<string>(options));
            control.onValueChanged.AddListener(changed);
            return control;
        }
        static Toggle Checkbox(Toggle source,Transform column,string label,UnityAction<bool> changed) {
            var row=Instantiate(source.transform.parent.gameObject,column,false);
            row.name=label;
            var control=row.GetComponentInChildren<Toggle>(true);
            control.group=null;control.onValueChanged=new Toggle.ToggleEvent();
            row.GetComponentInChildren<TMP_Text>(true).text=label;
            control.onValueChanged.AddListener(changed);
            return control;
        }
        public void Build(NuclearDLSS p,TMP_Dropdown dropdown,Toggle checkbox) {
            var left=PrepareColumn(transform.GetChild(0),"UPSCALING");
            var right=PrepareColumn(transform.GetChild(1),"FRAME GENERATION");
            srNote=Note(left);fgNote=Note(right);
            scaler=Dropdown(dropdown,left,"UPSCALER",new[]{"NVIDIA DLSS","AMD FSR 3.1"},i=>p.preferFsrScaling.Value=i==1);
            preset=Dropdown(dropdown,left,"SUPER RESOLUTION",new[]{"Off","DLAA (Native)","Quality","Balanced","Performance","Ultra Performance"},
                p.SelectQuality);
            model=Dropdown(dropdown,left,"DLSS MODEL",new[]{"NVIDIA Recommended","DLSS 4.5 (M / L)"},i=>p.forceSecondGen.Value=i==1);
            instruments=Checkbox(checkbox,left,"Native Instruments",v=>p.nativePanel.Value=v);
            reflex=Dropdown(dropdown,left,"NVIDIA REFLEX",new[]{"On","On + Boost"},i=>p.reflexBoost.Value=i==1);
            provider=Dropdown(dropdown,right,"FRAME GENERATION PROVIDER",providers,i=>p.provider.Value=providers[i]);
            gen=Checkbox(checkbox,right,"Frame Generation",v=>p.fg.Value=v);
            multi=Checkbox(checkbox,right,"Multi Frame Generation",v=>p.mfg.Value=v);
            factor=Dropdown(dropdown,right,"MULTIPLIER",new string[0],i=>{
                if(i<0 || i>=factors.Count) return;
                int n=factors[i];p.dynamic.Value=n==0;if(n>0)p.multiplier.Value=n;
            });
            refreshCap=Checkbox(checkbox,right,RefreshCapTitle,v=>p.capToRefresh.Value=v);
            refreshLabel=refreshCap.transform.parent.GetComponentInChildren<TMP_Text>(true);
            // Use the space up to the stock checkbox for fractional refresh rates.
            var labelRect=refreshLabel.rectTransform;
            labelRect.anchorMax=new Vector2(1,labelRect.anchorMax.y);
            labelRect.offsetMax=new Vector2(-52,labelRect.offsetMax.y);
            rates=Checkbox(checkbox,right,"Show FPS While Flying",v=>p.fpsOverlay.Value=v);
            mod=p;Refresh();
        }
        static void ShowNote(TMP_Text note,string message) {
            bool visible=!string.IsNullOrEmpty(message);
            if(visible && note.text!=message)note.text=message;
            if(note.gameObject.activeSelf!=visible)note.gameObject.SetActive(visible);
        }
        void LateUpdate(){if(mod)Refresh();}
        void Refresh() {
            bool ready=mod.state==1;
            scaler.SetValueWithoutNotify(mod.FsrScaling?1:0);
            scaler.interactable=ready && mod.HasDlssScaling && mod.HasFsrScaling;
            string native=mod.FsrScaling?"Native AA":"DLAA (Native)";
            if(preset.options[1].text!=native){preset.options[1].text=native;preset.RefreshShownValue();}
            ShowNote(srNote,ready?(mod.CanUpscale?null:"No compatible upscaler available"):mod.status);
            ShowNote(fgNote,mod.ProviderNote);
            provider.SetValueWithoutNotify(mod.ProviderIndex(mod.provider.Value));
            provider.interactable=ready && mod.CanSelectDLSS;
            if(mod.max!=previousMax || mod.dyn!=previousDynamic) {
                previousMax=mod.max;previousDynamic=mod.dyn;
                factors.Clear();var labels=new List<string>();
                for(int i=3;i<=Math.Min(6,mod.max+1);i++){factors.Add(i);labels.Add(i+"x");}
                if(mod.dyn!=0 && mod.max>1){factors.Add(0);labels.Add("Dynamic");}
                if(labels.Count==0)labels.Add("Unavailable");
                factor.ClearOptions();factor.AddOptions(labels);
            }
            int index=Array.IndexOf(modes,mod.ActiveQuality);
            if(preset.value!=index)preset.SetValueWithoutNotify(index);
            int modelIndex=mod.forceSecondGen.Value?1:0;
            if(model.value!=modelIndex)model.SetValueWithoutNotify(modelIndex);
            int reflexIndex=mod.reflexBoost.Value?1:0;
            if(reflex.value!=reflexIndex)reflex.SetValueWithoutNotify(reflexIndex);
            reflex.interactable=ready && mod.hasReflex && mod.max>0 && mod.fg.Value;
            refreshCap.interactable=reflex.interactable;
            refreshCap.SetIsOnWithoutNotify(mod.capToRefresh.Value);
            if(refreshLabel.text!=mod.refreshCapLabel)refreshLabel.text=mod.refreshCapLabel;
            index=Math.Max(0,factors.IndexOf(mod.dynamic.Value && mod.dyn!=0?0:Math.Min(mod.multiplier.Value,mod.max+1)));
            if(factor.value!=index)factor.SetValueWithoutNotify(index);
            instruments.SetIsOnWithoutNotify(mod.nativePanel.Value);gen.SetIsOnWithoutNotify(mod.fg.Value);
            multi.SetIsOnWithoutNotify(mod.mfg.Value);rates.SetIsOnWithoutNotify(mod.fpsOverlay.Value);
            preset.interactable=ready && mod.CanUpscale;
            model.interactable=ready && !mod.FsrScaling && mod.ActiveQuality!=Quality.Off;
            instruments.interactable=ready && !mod.vulkan && mod.ActiveQuality!=Quality.Off;
            gen.interactable=ready && mod.max>0;
            multi.interactable=gen.interactable && mod.fg.Value && mod.max>1;
            factor.interactable=multi.interactable && mod.mfg.Value;
            rates.interactable=ready;
        }
    }
}

