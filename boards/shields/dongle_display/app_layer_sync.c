/*
 * app_layer_sync.c
 * 
 * Module central de synchronisation bidirectionnelle entre Selenite (Mac) et le clavier ZMK.
 * 
 * DEUX DIRECTIONS DE COMMUNICATION :
 * ────────────────────────────────────
 *   Mac ──→ Clavier  : Raw HID output report (reçu via raw_hid_received_event)
 *                       byte[0] = calque cible, byte[1] = auto, byte[2] = one-shot, byte[3..] = nom app
 *                       Géré par : on_raw_hid_received()
 * 
 *   Clavier ──→ Mac  : Raw HID input report (envoyé via raw_hid_send())
 *                       byte[0] = calque actuellement actif sur le clavier
 *                       Déclenché par : tout changement de calque (zmk_layer_state_changed)
 *                       Géré par : send_active_layer_to_mac()
 * 
 * ARCHITECTURE DES CALQUES :
 * ─────────────────────────
 *   current_app_layer  : Le calque que le Mac a demandé (stocké en mémoire).
 *                        Peut être 0 (base) ou un numéro de calque.
 *                        Ce calque n'est PAS forcément actif physiquement.
 * 
 *   active_app_layer   : Le calque qui est REELLEMENT activé sur le clavier.
 *                        Vaut 0 quand aucun calque d'app n'est activé.
 *                        Seul ce calque est visible par l'utilisateur.
 * 
 *   is_layer_persistent : Si false, le calque se désactive après UNE frappe (one-shot).
 *                         Si true, le calque reste actif indéfiniment jusqu'à action manuelle.
 * 
 * MODES DE FONCTIONNEMENT :
 * ────────────────────────
 *   Mode Auto     : Le calque est activé IMMÉDIATEMENT à la réception du message Mac.
 *                   Pas besoin d'appuyer sur F13.
 * 
 *   Mode Manuel   : Le calque est seulement STOCKÉ en mémoire.
 *                   L'utilisateur doit appuyer sur F13 pour l'activer/désactiver.
 * 
 *   Mode One-shot : Le calque se désactive automatiquement après la prochaine frappe
 *                   (sauf les modificateurs : Cmd, Shift, Alt, Ctrl).
 *                   Compatible avec les modes Auto et Manuel.
 */

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>
#include <raw_hid/events.h>

/* ==========================================================================
 * VARIABLES GLOBALES
 * ========================================================================== */

// Le calque que le Mac nous a demandé d'activer.
// Peut rester en mémoire sans être actif (mode Manuel).
// Exemples : 0 = base (aucune app), 3 = calque Figma, 7 = calque Autocad
uint8_t current_app_layer = 0;

// Le calque qui est REELLEMENT actif sur le clavier en ce moment.
// 0 = aucun calque d'app activé (on est sur la base).
// >0 = un calque d'app est actuellement au-dessus de la base.
uint8_t active_app_layer = 0;

// Détermine si le calque doit rester activé après une frappe.
// false = mode one-shot : une seule frappe puis retour à la base.
// true  = mode persistant : le calque reste tant qu'on ne le désactive pas.
bool is_layer_persistent = false;


/* ==========================================================================
 * SECTION 1 : CLAVIER → MAC  (Envoi du calque actif)
 * ========================================================================== */

/*
 * send_active_layer_to_mac()
 * 
 * Envoie un rapport Raw HID de 32 octets au Mac avec le calque actuellement actif.
 * 
 * PROTOCOLE DU RAPPORT :
 *   byte[0]  = numéro du calque le plus haut actuellement actif (0 = base)
 *   byte[1..31] = réservé (mis à 0)
 * 
 * Le Mac (HIDManager.swift) écoute ce rapport via IOHIDManagerRegisterInputReportCallback
 * et met à jour l'overlay (bulle + barre de menus) avec le vrai calque actif.
 * 
 * UTILISÉ PAR : on_layer_state_changed() — appelé à CHAQUE changement de calque.
 */
static void send_active_layer_to_mac(void) {
    uint8_t report[32] = {0};

    // zmk_keymap_highest_layer_active() retourne l'index du calque le plus élevé
    // qui est actuellement actif. 0 = base, 1 = premier calque, etc.
    report[0] = zmk_keymap_highest_layer_active();

    // raw_hid_send() vient du module zmk-raw-hid.
    // Elle envoie un rapport HID input de 32 octets vers l'hôte (Mac).
    // Si le dongle USB n'est pas connecté, l'appel échoue silencieusement.
    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = report,
        .length = sizeof(report)
    });
}

/*
 * on_layer_state_changed()
 * 
 * Écouteur ZMK déclenché à CHAQUE changement de calque sur le clavier.
 * 
 * Cela inclut TOUTES les sources de changement :
 *   - Activation/désactivation par notre module (auto, F13, one-shot)
 *   - Activation par une touche "MO" (momentary) dans le keymap
 *   - Activation par une touche "TO" (toggle) dans le keymap
 *   - Toute autre source de changement de calque
 * 
 * À chaque changement, on informe le Mac du NOUVEAU calque actif.
 * Le Mac compare ce numéro avec ce qu'il pense et met à jour l'overlay.
 * 
 * ZMK_EV_EVENT_BUBBLE : On laisse l'événement continuer vers les autres écouteurs
 * (notamment le widget layer_status.c qui affiche le calque sur l'écran OLED du dongle).
 */
static int on_layer_state_changed(const zmk_event_t *eh) {
    // On informe le Mac du nouveau calque actif
    send_active_layer_to_mac();

    // On laisse l'événement se propager aux autres écouteurs ZMK
    return ZMK_EV_EVENT_BUBBLE;
}

// Enregistrement de l'écouteur auprès du système d'événements ZMK.
// "app_layer_report" est l'identifiant unique de cet écouteur.
ZMK_LISTENER(app_layer_report, on_layer_state_changed);
// On s'abonne aux changements de calque.
// zmk_layer_state_changed est émis par ZMK à chaque activation/désactivation de calque.
ZMK_SUBSCRIPTION(app_layer_report, zmk_layer_state_changed);


/* ==========================================================================
 * SECTION 2 : MAC → CLAVIER  (Réception des commandes de calque)
 * ========================================================================== */

/*
 * on_raw_hid_received()
 * 
 * Écouteur déclenché quand le Mac envoie un rapport Raw HID de 32 octets au clavier.
 * 
 * PROTOCOLE DU RAPPORT REÇU :
 *   byte[0] = numéro du calque cible (0 = retour à la base)
 *   byte[1] = mode auto (1 = activer immédiatement, 0 = stocker en mémoire)
 *   byte[2] = mode one-shot (1 = désactiver après une frappe, 0 = persistant)
 *   byte[3..31] = nom de l'application (texte UTF-8, pas utilisé ici mais disponible)
 * 
 * LOGIQUE DE TRAITEMENT :
 *   1. Désactiver l'ancien calque d'app s'il était actif
 *   2. Selon le mode (auto/manual) :
 *      - Auto : activer le nouveau calque immédiatement
 *      - Manuel : juste le stocker, l'utilisateur activera via F13
 *   3. Informer le Mac du calque réellement actif (via send_active_layer_to_mac)
 */
static int on_raw_hid_received(const zmk_event_t *eh) {
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    // --- Décodage du protocole ---
    uint8_t layer = ev->data[0];           // Calque cible demandé par le Mac
    bool is_auto = (ev->data[1] == 1);    // Mode auto : activation immédiate ?
    bool is_one_shot = (ev->data[2] == 1);// Mode one-shot : désactivation après 1 frappe ?
    
    // --- Étape 1 : Nettoyage de l'ancien calque ---
    // Si un calque d'app était activé, on le désactive AVANT d'en activer un nouveau.
    // Le paramètre "true" demande à ZMK de forcer la désactivation même si le calque
    // est actif via un mécanisme ZMK standard (MO, TO, etc.)
    if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
        zmk_keymap_layer_deactivate(active_app_layer, true);
    }
    
    // On réinitialise les états
    active_app_layer = 0;       // Plus aucun calque d'app actif
    is_layer_persistent = false; // Réinitialisation du mode persistant
    
    // --- Étape 2 : Traitement du nouveau calque ---
    if (layer == 0) {
        // ──────────────────────────────────────────────
        // CAS A : Retour à la base (aucune app spécifique)
        // ──────────────────────────────────────────────
        // Le Mac nous dit "aucun calque d'app à activer".
        // On réinitialise current_app_layer à 0.
        // Le calque actif retombe à 0 (base) grâce à la désactivation ci-dessus.
        current_app_layer = 0;
        
    } else if (is_auto) {
        // ──────────────────────────────────────────────
        // CAS B : Mode Auto — activation immédiate
        // ──────────────────────────────────────────────
        // Le Mac demande que le calque soit activé DIRECTEMENT, sans attendre F13.
        // Typiquement utilisé quand l'utilisateur focus une app avec "Auto" coché.
        
        zmk_keymap_layer_activate(layer, true); // Activation immédiate du calque
        active_app_layer = layer;               // On mémorise qu'il est actif
        current_app_layer = layer;               // On mémorise le calque demandé
        is_layer_persistent = !is_one_shot;      // Persistant sauf si one-shot
        
    } else {
        // ──────────────────────────────────────────────
        // CAS C : Mode Manuel — stockage en mémoire
        // ──────────────────────────────────────────────
        // Le Mac nous dit "voici le calque pour cette app" mais on NE l'active pas.
        // L'utilisateur devra appuyer sur F13 pour l'activer manuellement.
        // Utile pour les apps où on ne veut pas changer de calque automatiquement.
        
        current_app_layer = layer;               // On stocke le calque en mémoire
        is_layer_persistent = !is_one_shot;      // Persistant sauf si one-shot
    }
    
    // --- Étape 3 : Informer le Mac du résultat ---
    // Après avoir traité la commande, on envoie au Mac le calque RÉELLEMENT actif.
    // En mode auto, le Mac recevra le nouveau calque.
    // En mode manuel, le Mac recevra 0 (base) car on n'a rien activé.
    // IMPORTANT : on_change_state_changed sera aussi déclenché par les
    // zmk_keymap_layer_activate/deactivate ci-dessus, donc send_active_layer_to_mac
    // sera appelé automatiquement. Mais on l'appelle aussi ici pour le cas layer==0
    // où aucun changement de calque ZMK ne se produit.
    send_active_layer_to_mac();
    
    return ZMK_EV_EVENT_BUBBLE;
}


/* ==========================================================================
 * SECTION 3 : GESTION DU CLAVIER  (F13 toggle + One-shot désactivation)
 * ========================================================================== */

/*
 * on_keycode_state_changed()
 * 
 * Écouteur déclenché à CHAQUE touche pressée ou relâchée sur le clavier.
 * 
 * DEUX FONCTIONS :
 *   1. TOGGLE F13 : Si l'utilisateur appuie sur F13, on active/désactive
 *      le calque stocké dans current_app_layer (mode Manuel).
 * 
 *   2. ONE-SHOT : Si le calque actif est en mode one-shot (non persistant)
 *      et que l'utilisateur presse une touche ORDINAIRE (pas un modificateur),
 *      on désactive le calque après cette frappe.
 * 
 * CODES HID DES MODIFICATEURS (usage page 0x07) :
 *   0xE0 = Left Ctrl,  0xE1 = Left Shift,  0xE2 = Left Alt,  0xE3 = Left GUI (Cmd)
 *   0xE4 = Right Ctrl, 0xE5 = Right Shift, 0xE6 = Right Alt, 0xE7 = Right GUI
 */
static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    
    // On ignore les relâchements de touches et les événements invalides.
    // On ne réagit qu'aux APPUIS (state == true).
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // ──────────────────────────────────────────
    // FONCTION 1 : TOGGLE via F13 (keycode 0x68)
    // ──────────────────────────────────────────
    // F13 est la touche programmée dans le keymap pour activer/désactiver
    // le calque de l'app courante (mode Manuel).
    // Usage page 0x07 = Generic Desktop / Keyboard, keycode 0x68 = F13.
    if (ev->usage_page == 0x07 && ev->keycode == 0x68) {
        if (current_app_layer > 0 && active_app_layer == 0) {
            // Aucun calque actif mais un calque est en mémoire → on l'ACTIVE
            zmk_keymap_layer_activate(current_app_layer, true);
            active_app_layer = current_app_layer;
            // send_active_layer_to_mac() sera appelé par on_layer_state_changed
            // qui est déclenché par zmk_keymap_layer_activate
        } else if (active_app_layer > 0) {
            // Un calque est actif → on le DÉSACTIVE (retour à la base)
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
            // send_active_layer_to_mac() sera appelé par on_layer_state_changed
        }
        // ZMK_EV_EVENT_HANDLED : On consomme l'événement F13 pour éviter
        // qu'il soit traité comme une touche normale par ZMK.
        // Sinon, F13 serait aussi envoyé au Mac en tant que frappe clavier.
        return ZMK_EV_EVENT_HANDLED;
    }

    // ──────────────────────────────────────────
    // FONCTION 2 : ONE-SHOT (désactivation après 1 frappe)
    // ──────────────────────────────────────────
    // Si un calque d'app est actif ET qu'il est en mode non-persistant (one-shot),
    // on le désactive après la prochaine touche ordinaire.
    if (active_app_layer > 0 && !is_layer_persistent) {
        
        // Vérifie si la touche est un modificateur (Ctrl, Shift, Alt, Cmd/GUI).
        // Les modificateurs NE déclenchent PAS la désactivation du calque one-shot.
        // Cela permet de faire : Activer calque → Maintenir Cmd → Faire un raccourci
        // sans que le calque se désactive trop tôt.
        bool is_mod = (ev->usage_page == 0x07 && ev->keycode >= 0xE0 && ev->keycode <= 0xE7);
        
        if (!is_mod) {
            // Touche ordinaire : on désactive le calque one-shot
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
            // send_active_layer_to_mac() sera appelé par on_layer_state_changed
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}


/* ==========================================================================
 * ENREGISTREMENT DES ÉCOUTEURS ZMK
 * ========================================================================== */

// Écouteur 1 : Réception des commandes Raw HID depuis le Mac
// Identifiant : "app_layer_sync_hid"
// Événement écouté : raw_hid_received_event (émis par le module zmk-raw-hid)
ZMK_LISTENER(app_layer_sync_hid, on_raw_hid_received);
ZMK_SUBSCRIPTION(app_layer_sync_hid, raw_hid_received_event);

// Écouteur 2 : Détection des touches pressées (F13 toggle + one-shot)
// Identifiant : "app_layer_sync_key"
// Événement écouté : zmk_keycode_state_changed (émis par ZMK à chaque frappe)
ZMK_LISTENER(app_layer_sync_key, on_keycode_state_changed);
ZMK_SUBSCRIPTION(app_layer_sync_key, zmk_keycode_state_changed);