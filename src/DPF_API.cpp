#include "DPF_API.h"

namespace DPF {
    RE::TESForm* Create(RE::TESForm* baseTemplate) {
        if (!baseTemplate) {
            return nullptr;
        }
        
        SKSE::log::error("DPF::Create() should not be called directly - use via Papyrus callback in BookManager");
        return nullptr;
    }
    
    void Track(RE::TESForm* form) {
        // TODO: Implement if needed
        // For now, Create() is sufficient
    }
    
    void Dispose(RE::TESForm* form) {
        if (!form) {
            SKSE::log::warn("DPF::Dispose called with null form");
            return;
        }
        
        // Save FormID before we move the form
        auto formID = form->GetFormID();
        
        // Get the Papyrus VM
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            SKSE::log::error("Failed to get Papyrus VM for DPF::Dispose");
            return;
        }
        
        // Call DynamicPersistentForms.Dispose(Form)
        auto args = RE::MakeFunctionArguments(std::move(form));
        
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCallback;
        bool dispatched = vm->DispatchStaticCall("DynamicPersistentForms", "Dispose", args, nullCallback);
        
        if (dispatched) {
            SKSE::log::info("Disposed form 0x{:X} from DPF (prevents save crash)", formID);
        } else {
            SKSE::log::error("Failed to dispatch DPF::Dispose for form 0x{:X}", formID);
        }
    }
}
