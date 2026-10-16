#include <stdio.h>
#include <string.h>

#include "imports.h"
#include "log.h"
#include "macho.h"
#include "patch.h"

// Tablas de imports especificos del juego que se este portando.
// Agrega aqui las funciones del binario que desees interceptar o reemplazar.
const import_entry patch_imports[] = {
    // Ejemplo:
    // F(mi_funcion_personalizada, mi_implementacion),
};
const int patch_imports_count = sizeof(patch_imports) / sizeof(patch_imports[0]);

void patch_init(void) {
    l_info("Inicializando parches especificos del juego...");
    // Coloca aqui tus llamadas a macho_hook(...) si necesitas desviar funciones internas
}

void patch_frame(void) {
    // Logica por frame si el juego lo requiere
}
