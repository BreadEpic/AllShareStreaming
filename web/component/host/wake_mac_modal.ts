import { getCurrentLanguage, getTranslations } from "../../i18n"
import { InputComponent } from "../input"
import { FormModal } from "../modal/form"

// Returns the new mac address, an empty string removes the manually set one
export class WakeMacModal extends FormModal<string> {

    private header: HTMLElement = document.createElement("h2")
    private description: HTMLElement = document.createElement("p")

    private mac: InputComponent
    private currentMac: string

    constructor(currentMac: string | null, reportedMac: string | null) {
        super()
        this.currentMac = currentMac ?? ""
        const i = getTranslations(getCurrentLanguage()).host

        this.header.innerText = i.wakeMacHeader
        this.description.innerText = i.wakeMacDescription(reportedMac)

        this.mac = new InputComponent("wakeMac", "text", i.wakeMacLabel, {
            placeholer: "AA:BB:CC:DD:EE:FF",
        })
    }

    reset(): void {
        this.mac.setValue(this.currentMac)
    }
    submit(): string | null {
        return this.mac.getValue().trim()
    }

    mountForm(form: HTMLFormElement): void {
        form.appendChild(this.header)
        form.appendChild(this.description)
        this.mac.mount(form)
    }
}

// Same formats as the server accepts: AA:BB:CC:DD:EE:FF, aa-bb-cc-dd-ee-ff, aabb.ccdd.eeff, aabbccddeeff
export function isValidMac(mac: string): boolean {
    const hex = mac.trim().replace(/[:\-. ]/g, "")

    return /^[0-9a-fA-F]{12}$/.test(hex) && !/^(0{12}|[fF]{12})$/.test(hex)
}
