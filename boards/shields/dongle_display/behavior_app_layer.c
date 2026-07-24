/*
 * behavior_app_layer.c
 * 
 * Comportement ZMK personnalisé : &app_layer
 * 
 * Permet d'utiliser &app_layer directement dans le keymap (.keymap)
 * pour basculer (toggle) le calque de l'app courante.
 * 
 * Alternative à &kp F13 : même fonctionnalité mais plus propre
 * dans le keymap car sémantiquement plus clair.
 * 
 * EXEMPLE DANS LE KEYMAP :
 *   &app_layer    ← un appui active le calque, un second le désactive
 * 
 * ⚠️ NOTE IMPORTANTE :
 * Ce fichier fournit UNIQUEMENT le comportement &app_layer.
 * La gestion du one-shot, F13, et l'envoi au Mac sont tous
 * gérés par app_layer_sync.c. Ce fichier NE doit PAS dupliquer
 * cette logique sous peine de conflits.
 */

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>

#define DT_DRV_COMPAT zmk_behavior_app_layer

/*
 * Variable globale définie dans app_layer_sync.c.
 * C'est le calque que le Mac a demandé pour l'app courante.
 * On la lit seulement — on ne la modifie jamais depuis ce fichier.
 */
extern uint8_t current_app_layer;

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * Initialisation du driver de comportement.
 * Obligatoire pour tout behavior ZMK, même si on n'a rien à initialiser.
 */
static int behavior_app_layer_init(const struct device *dev) { return 0; };

/*
 * on_keymap_binding_pressed()
 * 
 * Appelée quand l'utilisateur APPUIE sur une touche assignée à &app_layer.
 * 
 * LOGIQUE DE TOGGLE :
 *   - Si current_app_layer > 0 (le Mac a assigné un calque) :
 *       → Si le calque est déjà actif → on le DÉSACTIVE
 *       → Si le calque n'est pas actif → on l'ACTIVE
 *   - Si current_app_layer == 0 (pas d'app avec calque) :
 *       → On ne fait rien (ZMK_BEHAVIOR_OPAQUE)
 * 
 * IMPORTANT : L'envoi du calque actif au Mac sera fait automatiquement
 * par app_layer_sync.c qui écoute zmk_layer_state_changed.
 * Ici on ne fait qu'activer/désactiver le calque localement.
 */
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (current_app_layer > 0) {
        if (zmk_keymap_layer_active(current_app_layer)) {
            return zmk_keymap_layer_deactivate(current_app_layer, true);
        } else {
            return zmk_keymap_layer_activate(current_app_layer, true);
        }
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

/*
 * on_keymap_binding_released()
 * 
 * Appelée quand l'utilisateur RELÂCHE la touche &app_layer.
 * Ne fait rien — c'est un toggle, pas un momentary.
 */
static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

/*
 * Table des fonctions du driver.
 * Obligatoire : ZMK appelle binding_pressed/binding_released via cette table.
 */
static const struct behavior_driver_api behavior_app_layer_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

/* ==========================================================================
 * ENREGISTREMENT DU DRIVER ZMK (standard pour tout comportement custom)
 * ========================================================================== */

#define KP_INST(n)                                                                                   \
    DEVICE_DT_INST_DEFINE(n, behavior_app_layer_init, NULL, NULL, NULL, POST_KERNEL,                 \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_app_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif