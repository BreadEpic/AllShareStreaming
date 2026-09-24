import { DetailedHost, DetailedUser, UndetailedHost } from "../../api_bindings"
import { Api, apiDeleteHost, apiGetHost, isDetailedHost, apiPostPair, apiWakeUp, apiGetUser, apiPatchHost, FetchError } from "../../api"
import { Component, ComponentEvent } from "../index"
import { getCurrentLanguage, getTranslations } from "../../i18n"
import { setContextMenu } from "../context_menu"
import { showNotification } from "../notification"
import { showMessage, showModal } from "../modal/index"
import { HOST_IMAGE, HOST_OVERLAY_LOCK, HOST_OVERLAY_NONE, HOST_OVERLAY_OFFLINE } from "../../resources/index"
import { isValidMac, WakeMacModal } from "./wake_mac_modal"

export type HostEventListener = (event: ComponentEvent<Host>) => void

// How long to wait for a pc to wake up before giving up
const WAKE_TIMEOUT_MS = 2 * 60 * 1000
const WAKE_POLL_INTERVAL_MS = 3000
// Some network cards miss the first packets while going into sleep -> send them again once
const WAKE_RESEND_AFTER_MS = 20 * 1000

export class Host implements Component {
    private api: Api

    private hostId: number
    private userCache: DetailedUser | null = null
    private cache: UndetailedHost | DetailedHost | null = null

    private divElement: HTMLDivElement = document.createElement("div")

    private imageElement: HTMLImageElement = document.createElement("img")
    private imageOverlayElement: HTMLImageElement = document.createElement("img")
    private nameElement: HTMLElement = document.createElement("p")

    private waking: boolean = false

    constructor(api: Api, hostId: number, host: UndetailedHost | DetailedHost | null) {
        this.api = api

        this.hostId = hostId
        this.cache = host

        // Configure image
        this.imageElement.classList.add("host-image")
        this.imageElement.src = HOST_IMAGE

        // Configure image overlay
        this.imageOverlayElement.classList.add("host-image-overlay")

        // Configure name
        this.nameElement.classList.add("host-name")

        // Append elements
        this.divElement.appendChild(this.imageElement)
        this.divElement.appendChild(this.imageOverlayElement)
        this.divElement.appendChild(this.nameElement)

        this.divElement.addEventListener("click", this.onClick.bind(this))
        this.divElement.addEventListener("contextmenu", this.onContextMenu.bind(this))

        // Update cache
        if (host != null) {
            this.updateCache(host, null)

            apiGetUser(api).then((user) => this.userCache = user)
        } else {
            this.forceFetch()
        }
    }

    async forceFetch() {
        const [newCache, user] = await Promise.all([
            apiGetHost(this.api, {
                host_id: this.hostId,
            }),
            apiGetUser(this.api)
        ])

        this.updateCache(newCache, user)
    }
    async getCurrentGame(): Promise<number | null> {
        await this.forceFetch()

        if (this.cache && isDetailedHost(this.cache) && this.cache.current_game != 0) {
            return this.cache.current_game
        } else {
            return null
        }
    }

    private async onClick(event: MouseEvent) {
        if (this.cache?.server_state == null) {
            if (this.cache?.can_wake) {
                // Clicking a sleeping pc wakes it up and opens it when it's online
                await this.wakeUp()
            } else {
                this.onContextMenu(event)
            }
        } else if (this.cache?.paired == "Paired") {
            this.divElement.dispatchEvent(new ComponentEvent("ml-hostopen", this))
        } else {
            await this.pair()
        }
    }

    private onContextMenu(event: MouseEvent) {
        const i = getTranslations(getCurrentLanguage()).host
        const elements = []

        if (this.cache?.server_state != null) {
            elements.push({
                name: i.showDetails,
                callback: this.showDetails.bind(this),
            })

            elements.push({
                name: i.open,
                callback: this.onClick.bind(this)
            })
        } else if (this.cache?.can_wake || this.cache?.paired == "Paired") {
            elements.push({
                name: i.wakeUp,
                callback: this.wakeUp.bind(this)
            })
        }

        elements.push({
            name: i.reload,
            callback: async () => this.forceFetch()
        })

        if (this.cache?.owner == "ThisUser" || this.userCache?.role == "Admin") {
            elements.push({
                name: i.setWakeMac,
                callback: this.setWakeMac.bind(this)
            })
        }

        if (this.cache?.server_state != null && this.cache?.paired == "NotPaired") {
            elements.push({
                name: i.pair,
                callback: this.pair.bind(this)
            })
        }

        // Make private / global
        if (this.userCache?.role == "Admin") {
            if (this.cache?.owner == "Global") {
                elements.push({
                    name: i.makePrivate,
                    callback: this.makePrivate.bind(this),
                    classes: ["context-menu-element-red"]
                })
            } else if (this.cache?.owner == "ThisUser") {
                elements.push({
                    name: i.makeGlobal,
                    callback: this.makeGlobal.bind(this),
                    classes: ["context-menu-element-red"]
                })
            }
        }

        if (this.cache?.owner == "ThisUser" || this.userCache?.role == "Admin") {
            elements.push({
                name: i.removeHost,
                callback: this.remove.bind(this)
            })
        }

        setContextMenu(event, {
            elements
        })
    }

    private async showDetails() {
        const i = getTranslations(getCurrentLanguage()).host
        let host = this.cache;
        if (!host || !isDetailedHost(host)) {
            host = await apiGetHost(this.api, {
                host_id: this.hostId,
            })
        }
        if (!host || !isDetailedHost(host)) {
            showNotification(i.failedToGetDetails(this.hostId))
            return;
        }
        this.updateCache(host, this.userCache)

        await showMessage(i.details(host))
    }

    addHostRemoveListener(listener: HostEventListener, options?: EventListenerOptions) {
        this.divElement.addEventListener("ml-hostremove", listener as any, options)
    }
    removeHostRemoveListener(listener: HostEventListener, options?: EventListenerOptions) {
        this.divElement.removeEventListener("ml-hostremove", listener as any, options)
    }

    addHostOpenListener(listener: HostEventListener, options?: EventListenerOptions) {
        this.divElement.addEventListener("ml-hostopen", listener as any, options)
    }
    removeHostOpenListener(listener: HostEventListener, options?: EventListenerOptions) {
        this.divElement.removeEventListener("ml-hostopen", listener as any, options)
    }

    private async makeGlobal() {
        await apiPatchHost(this.api, {
            host_id: this.hostId,
            change_owner: true,
            owner: null,
        })

        if (this.cache) {
            this.cache.owner = "Global"
        }
    }
    private async makePrivate() {
        const user = this.userCache ?? await apiGetUser(this.api)

        await apiPatchHost(this.api, {
            host_id: this.hostId,
            change_owner: true,
            owner: user.id,
        })

        if (this.cache) {
            this.cache.owner = "ThisUser"
        }
    }

    private async remove() {
        await apiDeleteHost(this.api, {
            host_id: this.getHostId()
        })

        this.divElement.dispatchEvent(new ComponentEvent("ml-hostremove", this))
    }
    private async sendWakeUp(): Promise<boolean> {
        const i = getTranslations(getCurrentLanguage()).host
        try {
            await apiWakeUp(this.api, {
                host_id: this.getHostId()
            })
            return true
        } catch (e) {
            if (e instanceof FetchError && e.getResponse()?.status == 409) {
                // The server doesn't know the mac address
                await showMessage(i.wakeMacUnknown)
                return false
            }
            throw e
        }
    }
    private async wakeUp() {
        const i = getTranslations(getCurrentLanguage()).host
        const name = this.cache?.name ?? ""

        if (this.waking) {
            showNotification(i.alreadyWaking(name), "info")
            return
        }

        if (!await this.sendWakeUp()) {
            return
        }

        this.setWaking(true)
        showNotification(i.wakingUp(name), "info")

        const start = Date.now()
        let resent = false
        try {
            while (Date.now() - start < WAKE_TIMEOUT_MS) {
                await new Promise(resolve => setTimeout(resolve, WAKE_POLL_INTERVAL_MS))

                try {
                    await this.forceFetch()
                } catch (e) {
                    console.debug("host still not reachable", e)
                }

                if (this.cache?.server_state != null) {
                    showNotification(i.wakeUpSuccess(name), "info")

                    // Open it if the user is still looking at the host list
                    if (this.divElement.isConnected && this.cache.paired == "Paired") {
                        this.divElement.dispatchEvent(new ComponentEvent("ml-hostopen", this))
                    }
                    return
                }

                if (!resent && Date.now() - start > WAKE_RESEND_AFTER_MS) {
                    resent = true
                    await this.sendWakeUp().catch(e => console.warn("failed to resend wake up packet", e))
                }
            }

            showNotification(i.wakeUpTimeout(name), "warn")
        } finally {
            this.setWaking(false)
        }
    }
    private setWaking(waking: boolean) {
        this.waking = waking
        this.divElement.classList.toggle("host-waking", waking)
    }
    private async setWakeMac() {
        const i = getTranslations(getCurrentLanguage()).host

        const host = await apiGetHost(this.api, {
            host_id: this.hostId,
        })
        this.updateCache(host, this.userCache)

        const reportedMac = host.mac && host.mac != "00:00:00:00:00:00" ? host.mac : null
        const newMac = await showModal(new WakeMacModal(host.wake_mac, reportedMac))
        if (newMac == null) {
            return
        }
        if (newMac != "" && !isValidMac(newMac)) {
            await showMessage(i.wakeMacInvalid)
            return
        }

        await apiPatchHost(this.api, {
            host_id: this.hostId,
            change_owner: false,
            owner: null,
            wake_mac: newMac,
        })
        await this.forceFetch()

        showNotification(i.wakeMacSaved, "info")
    }
    private async pair() {
        const i = getTranslations(getCurrentLanguage()).host
        if (this.cache?.paired == "Paired") {
            await this.forceFetch()

            if (this.cache?.paired == "Paired") {
                showMessage(i.alreadyPaired)
                return;
            }
        }

        const responseStream = await apiPostPair(this.api, {
            host_id: this.getHostId()
        })

        if (typeof responseStream.response == "string") {
            throw `failed to pair (stage 1): ${responseStream.response}`
        }

        const messageAbort = new AbortController()
        showMessage(i.pairPrompt(this.getCache()?.name ?? "", responseStream.response.Pin), { signal: messageAbort.signal })

        const resultResponse = await responseStream.next()
        messageAbort.abort()

        if (!resultResponse) {
            throw "missing stage 2 of pairing"
        } else if (typeof resultResponse == "string") {
            throw `failed to pair (stage 2): ${resultResponse}`
        }

        this.updateCache(resultResponse.Paired, null)
    }

    getHostId(): number {
        return this.hostId
    }

    getCache(): DetailedHost | UndetailedHost | null {
        return this.cache
    }

    updateCache(host: UndetailedHost | DetailedHost, user: DetailedUser | null) {
        const i = getTranslations(getCurrentLanguage()).host
        if (this.getHostId() != host.host_id) {
            showNotification(i.overwriteMismatch(this.getHostId(), host.host_id))
            return
        }

        if (this.cache == null) {
            this.cache = host
        } else {
            // if server_state == null it means this host is offline
            // -> updating cache means setting it to offline
            if (this.cache.server_state != null) {
                Object.assign(this.cache, host)
            } else {
                this.cache = host
            }
        }

        if (user) {
            this.userCache = user
        }

        // Update Elements
        this.nameElement.innerText = this.cache.name

        if (this.cache.server_state == null) {
            this.imageOverlayElement.src = HOST_OVERLAY_OFFLINE
        } else if (this.cache.paired != "Paired") {
            this.imageOverlayElement.src = HOST_OVERLAY_LOCK
        } else {
            this.imageOverlayElement.src = HOST_OVERLAY_NONE
        }
    }

    mount(parent: HTMLElement): void {
        parent.appendChild(this.divElement)
    }
    unmount(parent: HTMLElement): void {
        parent.removeChild(this.divElement)
    }
}
