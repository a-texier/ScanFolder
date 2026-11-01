# ScanFolder : guide détaillé

Application Windows native pour trouver les dossiers et fichiers lourds, gérer des fichiers et créer des ZIP rapidement. Interface en français, thèmes sombre et clair.

## Cache par emplacement

- Changer de disque, saisir un chemin puis Entrée, parcourir ou déposer un dossier **ne déclenche pas de scan**. L’application ouvre l’index mémorisé. Un emplacement jamais analysé affiche une invitation à cliquer sur **Analyser**.
- Chaque disque et chaque racine analysée possède son index compressé et sa position mémorisée. Un aller-retour C: → D: → C: retrouve les analyses précédentes. **Analyser** (ou son raccourci F5) reconstruit seulement l’index choisi.
- Les écritures sont sérialisées et le chargement attend leur fin en arrière-plan : changer rapidement d’emplacement ne perd pas le dernier enregistrement. Les volumes locaux sont identifiés par leur GUID Windows pour distinguer deux disques utilisant la même lettre.

## Navigation, sélection et cache

- **Boutons latéraux de souris** : précédent / suivant. Équivalents clavier **Alt+Gauche / Alt+Droite** et boutons **← / →** au-dessus de la liste. **↑**, Retour ou Alt+Haut remontent au parent. L’historique retrouve le dossier, le filtre, le tri, le défilement et la sélection.
- **Ctrl+clic** ajoute ou retire une ligne. **Maj+clic** sélectionne une plage ; **Ctrl+Maj+clic** ajoute cette plage. **Ctrl+A** sélectionne les éléments affichés. Maj+flèches étend aussi la sélection. Un clic droit sur une ligne déjà sélectionnée conserve la sélection multiple.
- La corbeille, la suppression définitive, les copies et les déplacements concernent toute la sélection. La confirmation de suppression liste les chemins et les totaux ; la suppression définitive exige toujours `SUPPRIMER`. Un dossier sélectionné et ses descendants ne sont pas supprimés deux fois.
- Après suppression, copie, déplacement ou ZIP : **pas de rescan complet**, pas de retour à la racine. Mise à jour locale des emplacements concernés, sans parcourir les autres dossiers ; défilement, filtre et tri restent en place. Les opérations réussies sont signalées en bas sans boîte « OK » supplémentaire. F5 reste une analyse complète volontaire, en conservant le dossier affiché lorsqu’il existe encore.
- **Cache disque compressé** dans `%LOCALAPPDATA%\ScanFolder\index-*.db`, un index par racine analysée. Ces fichiers binaires sont propres à ScanFolder, pas des bases SQLite. Ils contiennent les métadonnées et noms, jamais le contenu des fichiers. Encodage compact, compression Windows XPRESS Huffman, contrôles CRC et vérification de l’arborescence au chargement. Aucun runtime supplémentaire.
- Le dernier emplacement est rechargé automatiquement au lancement, sans reparcourir les fichiers. Chaque racine retrouve son dossier, filtre, tri et défilement via `index-*.session` ; `session.dat` mémorise le dernier emplacement. Un scan complet terminé remplace seulement son index de manière atomique ; un scan annulé conserve le cache précédent. Les opérations locales mettent également l’index actif à jour en arrière-plan. L’ancien `cache.db` reste lisible pour retrouver la dernière analyse de la version 1.1.
- **Vider cache · taille** dans la barre latérale supprime le cache disque après confirmation. Les fichiers utilisateurs et l’index déjà chargé en mémoire restent présents. Une nouvelle analyse ou opération locale peut recréer le cache.

Le cache est un instantané : les changements faits par d’autres applications nécessitent Analyser ou F5. « Vider cache » retire tous les index et sessions mémorisés. La taille varie avec le nombre et les noms des fichiers ; le cache n’est pas chiffré. La variable facultative `SCANFOLDER_CACHE_DIR` permet de choisir son dossier (notamment pour les tests isolés). Seul l’index actif occupe la mémoire ; revenir à un autre disque recharge sa base compressée.

## Lancer

L’EXE est autonome : aucun runtime .NET, Python, Node, navigateur ou 7-Zip à installer. Windows 10/11 x64 ; les DLL système Windows restent nécessaires. Aucun compte, SDK de télémétrie, service réseau, téléchargement ou mise à jour automatique.

Le moteur 7-Zip et ses sources/licences sont embarqués. Pour la compression, l’EXE extrait temporairement `7za.exe` dans un dossier unique de `%TEMP%`, lance le processus sans console, puis nettoie ses fichiers temporaires. « Un seul EXE à distribuer » ne signifie donc pas « aucune écriture temporaire ».

## Utilisation

- Choisir un dossier, cliquer sur un disque, saisir un chemin ou déposer un dossier. Cliquer sur **Analyser**.
- **Dossiers** : sous-dossiers et fichiers immédiats, triables par nom, taille, nombre de fichiers ou date. Double-clic sur un dossier pour y entrer ; Retour pour remonter.
- **Gros fichiers** : tous les fichiers du sous-arbre, triés par taille. **Types** : tailles cumulées par extension. Double-clic sur une extension pour afficher ses fichiers.
- Recherche par fragment de nom, sans distinction de casse. Les tris, filtres et regroupements s’exécutent en arrière-plan ; seules les lignes visibles sont dessinées.
- Sélectionner une ligne pour ouvrir l’Explorateur, copier/couper, choisir une destination, créer un ZIP ou supprimer. Toutes les actions sont aussi dans le menu contextuel.
- **Ctrl+C / Ctrl+X / Ctrl+V** : copier / couper / coller dans le dossier actuellement affiché. Le presse-papiers est compatible avec l’Explorateur Windows, y compris plusieurs éléments provenant de celui-ci.
- Le menu contextuel propose **Copier le chemin**.
- **Ctrl+F** : recherche ; **Ctrl+L** : chemin ; **F5** : relancer ; **Suppr** : corbeille ; **Maj+Suppr** : suppression définitive ; **Échap** : arrêter une opération lorsque la fenêtre principale a le focus.
- Export CSV en UTF-8 avec colonnes numériques en octets.

## Performances et disques de 2 To ou plus

La capacité de 2 To n’est pas une limite de l’application : tailles, offsets et compteurs d’octets utilisent des entiers 64 bits. Le temps de scan dépend principalement du nombre d’entrées, des latences du disque, des droits d’accès et du cache Windows.

- **Turbo NTFS** : lecture du volume en accès strictement **lecture seule**, puis lecture des enregistrements de la MFT et reconstruction du sous-arbre. Nécessite un accès administrateur. Ne lit pas le contenu des fichiers. Si la MFT est répartie dans des attributs d’extension non pris en charge, ou si l’accès au volume échoue, repli vers le parcours standard.
- **Parcours standard** : `FindFirstFileExW` / `FindNextFileW`, lecture de métadonnées, file de dossiers partagée et 1 à 8 workers. Convient également à exFAT, FAT et aux partages accessibles via Windows.
- **HDD mécanique / USB lent** : commencer avec **1 ou 2 workers**. Sur SSD, **4** constitue le réglage initial. Le bouton workers permet de changer le nombre avant la prochaine opération. Ce réglage n’est pas automatique : augmenter le parallélisme peut ralentir un HDD.
- L’index est conservé en RAM. Un petit EXE ne rend pas gratuit le stockage de millions de noms. Les nœuds utilisent des indices 32 bits ; cette application vise des millions d’entrées, pas plusieurs milliards.

Un scan est un instantané non atomique. Les fichiers peuvent changer pendant l’analyse. Les jonctions et répertoires liés ne sont pas parcourus. Les flux de données alternatifs NTFS ne sont pas inclus. La somme affichée n’est pas forcément l’intégralité de l’espace utilisé du volume (métadonnées, réserves système, instantanés, éléments inaccessibles).

**Taille logique** est la vue initiale. En parcours standard, l’allocation est une **estimation** par clusters, corrigée pour certains fichiers compressés/sparse/cloud ; les petits fichiers résidents et les liens physiques peuvent expliquer des écarts. En mode MFT, les liens physiques conservent leurs différents noms, mais leur allocation n’est additionnée qu’une fois dans le sous-arbre choisi.

## Suppression

**Une confirmation est toujours demandée.** La suppression définitive demande en plus de saisir exactement **`SUPPRIMER`**. La boîte indique le chemin et les nombres issus du dernier scan ; la suppression d’un dossier concerne son contenu réel au moment de l’opération.

- La suppression définitive s’effectue sur des handles Windows, en arrière-plan, avec **1 à 8 workers**. Les fichiers sont traités en parallèle ; les dossiers sont ensuite supprimés par profondeur, enfants avant parents.
- La file de fichiers est bornée. Les répertoires sont mémorisés pour respecter l’ordre de suppression.
- Les chemins réels sont contrôlés. Les racines de volume, les racines de profil, Windows et ses descendants, ainsi que le dossier contenant l’application active sont bloqués.
- Les jonctions/liens sont supprimés comme des entrées ; leur cible n’est pas parcourue. Les fichiers ouverts sans partage de suppression restent en place et sont signalés. Les erreurs ne sont pas présentées comme une réussite complète.
- **Arrêter** interrompt le travail restant. Les éléments déjà supprimés ne sont pas restaurés. L’app ne force pas la fermeture des logiciels qui verrouillent des fichiers.
- **Corbeille** utilise le moteur Windows avec un contrôle préalable de chaque suppression : si l’élément ne peut pas être recyclé, l’opération est annulée, sans repli silencieux vers une suppression définitive.

« Définitive » signifie **hors corbeille**, pas effacement sécurisé des secteurs ou garantie de non-récupérabilité.

## Copies et déplacements

Copier vers, Déplacer vers et Coller demandent une confirmation avec les sources et la destination. Ils utilisent `IFileOperation` de Windows, avec progression, annulation et signalement des erreurs. Les noms en conflit sont renommés pour préserver les éléments existants. Un déplacement entre disques passe par une copie puis le retrait de la source par Windows ; il n’y a pas de vérification cryptographique supplémentaire dans ScanFolder. Les opérations interrompues peuvent avoir déjà copié ou déplacé certains éléments.

Le nombre de workers choisi s’applique au scan standard et à la suppression définitive. Les copies/déplacements restent pilotés par le moteur Windows ; ScanFolder ne lance pas plusieurs copies concurrentes sur un HDD.

## ZIP rapide

Moteur autonome **7-Zip 26.04 x64**, ZIP Deflate niveau 1, threads demandés selon le réglage workers. Le moteur choisit le parallélisme applicable au format. ZIP64 est géré par 7-Zip pour les grandes archives.

1. Sélectionner un fichier ou dossier, puis **Compresser en ZIP…**.
2. Choisir un emplacement **en dehors du dossier source**. Le remplacement d’un ZIP existant demande confirmation dans la boîte d’enregistrement.
3. ScanFolder prépare la liste, crée une archive temporaire à côté de la destination, puis la teste avec `7za t` (CRC).
4. La destination n’est publiée/remplacée qu’après une sortie sans avertissement du moteur et une vérification réussie. Le dossier source n’est jamais supprimé.

Les liens, jonctions et éléments cloud portant un attribut reparse sont exclus et leur nombre est annoncé. En cas de fichier inaccessible ou d’avertissement 7-Zip, le ZIP incomplet n’est pas publié. En cas d’annulation, l’archive temporaire de cette opération est supprimée. Si seule la publication finale échoue, le chemin du ZIP déjà vérifié et conservé est affiché. Une panne brutale peut laisser des fichiers temporaires.

## Construire depuis les sources

Prérequis : MSVC / SDK Windows, Visual Studio 2022 (ou Build Tools) avec les outils C++ x64. Lancer `build.cmd` depuis ce dossier. Il utilise `/O2 /GL /MT`, l’édition de liens statique du runtime C++, les ressources intégrées et les DLL de Windows. Fermer l’EXE final avant de reconstruire.

Les fichiers `third_party/7zip/extra/x64/7za.exe`, `License.txt` et `7z2604-src.tar.xz` sont nécessaires pour les ressources. Ils proviennent des distributions officielles :

- https://github.com/ip7z/7zip/releases/download/26.04/7z2604-extra.7z
- https://github.com/ip7z/7zip/releases/download/26.04/7z2604-src.tar.xz

Le code ScanFolder est indépendant ; aucun code de WinDirStat n’a été repris. 7-Zip est distribué séparément comme programme embarqué, sous sa licence GNU LGPL 2.1 ou ultérieure ; voir `third_party/7zip/extra/License.txt`. Pour employer un moteur 7za modifié, définir **`SCANFOLDER_7ZA`** vers son EXE avant le lancement. L’EXE permet d’extraire le moteur non modifié, sa licence et ses sources dans un dossier existant ou à créer :

```powershell
.\ScanFolder.exe --third-party "C:\chemin\dossier-neuf"
```

Cette commande ne remplace pas les fichiers existants.

## Vérifications reproductibles

```powershell
Start-Process .\ScanFolder.exe -ArgumentList '--self-test', 'self-test.txt' -Wait
Start-Process .\ScanFolder.exe -ArgumentList '--scan', '"C:\dossier"', '"C:\rapport.csv"', '--standard' -Wait
```

Le test crée uniquement un dossier unique dans `%TEMP%`, y génère 24 001 fichiers et ses autres fixtures, puis nettoie ses propres fichiers. Il vérifie les tailles, noms Unicode, chemins longs, jonctions, fichiers en lecture seule/verrouillés, annulations, protections de racines, vues, copies/déplacements, ZIP et CRC. Le lecteur MFT est testé avec un volume synthétique en mémoire (>4 Gio, liens physiques, jonction et MFT non prise en charge).
