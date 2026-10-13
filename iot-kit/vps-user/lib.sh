# Fonctions communes aux scripts de déploiement (à sourcer depuis le dossier iot-kit).

die() { echo "ERREUR : $*" >&2; exit 1; }
warn() { echo "ATTENTION : $*" >&2; }
step() { echo; echo "==> $*"; }

# Lit une valeur de .env sans exécuter le fichier ; retire les guillemets simples.
env_get() {
  grep -E "^$1=" .env | tail -n1 | cut -d= -f2- | sed -e "s/^'//" -e "s/'\$//"
}

# Remplace ou ajoute une valeur dans .env.
env_set() {
  if grep -qE "^$1=" .env; then
    sed -i "s|^$1=.*|$1=$2|" .env
  else
    echo "$1=$2" >> .env
  fi
}
