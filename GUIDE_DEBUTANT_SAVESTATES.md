# Savestates dans CEmulador : guide pour débutants

## 1. Obtenir le programme sans rien installer (via GitHub)

1. Créez un compte sur github.com si besoin, puis ouvrez le dépôt de votre fork de CEmulador.
2. Cliquez sur **Add file > Upload files** et déposez le contenu de ce dossier (ou remplacez les fichiers du dépôt par ceux du zip). Validez avec **Commit changes**.
3. Ouvrez l'onglet **Actions**. Le workflow **Build check** se lance seul après un envoi sur la branche `main`. Vous pouvez aussi le lancer à la main : **Build check > Run workflow**.
4. Attendez la fin (environ 30 à 60 minutes la première fois). Une coche verte = réussi.
5. Cliquez sur l'exécution terminée, descendez à **Artifacts** et téléchargez **cemu-bin-windows-x64**. Décompressez : vous obtenez `Cemu.exe`.
6. Si une croix rouge apparaît : ouvrez l'étape en échec, copiez les lignes `error:` et envoyez-les pour correction. Aucune compilation n'a pu être testée avant livraison, donc quelques corrections sont possibles.

## 2. Utiliser les savestates

- Lancez un jeu, puis menu **Savestates**.
- **Save state > Slot N** sauvegarde, **Load state > Slot N** charge. Le menu affiche la date de chaque slot, ou « empty ».
- Raccourcis par défaut : **Shift+F1 à F10** sauvegardent, **Ctrl+F1 à F10** chargent. Modifiables dans les réglages des raccourcis.
- **Export state to file...** et **Import state from file...** permettent d'enregistrer ou de charger un fichier `.cemustate` n'importe où sur le PC.
- Les slots sont dans le dossier utilisateur de Cemu : `savestates/<identifiant du jeu>/slot_N.cess`.

## 3. Messages que vous pouvez voir

- « a few seconds / try again » : le jeu était occupé (accès fichier, callback). Le programme réessaie seul pendant 3 secondes. Si ça persiste, réessayez.
- « online features are enabled » : désactivez les fonctions en ligne dans les réglages. Les savestates en ligne sont interdites.
- « another build/settings » : le fichier vient d'une autre version du programme, d'autres graphic packs ou d'un autre mode CPU.
- « different set of loaded game modules » : fermez le jeu, relancez-le, avancez jusqu'à peu près le même endroit, puis chargez.

## 4. Limites connues (honnêtes)

- Un savestate ne fonctionne qu'avec **exactement le même Cemu.exe** (même compilation). Pour partager des states dans la communauté, tout le monde doit utiliser le même exécutable.
- Le contenu des fichiers de sauvegarde du jeu (sur le disque) n'est pas remis en arrière.
- Pas de miniature d'aperçu.
- Certains services système émulés (compte, boss, USB Skylanders, amiibo, audio) gardent un état côté hôte qui n'est pas encore sauvegardé. Un jeu qui s'en sert fortement peut mal réagir après un chargement.
- Rien n'a encore été testé sur un vrai jeu. Commencez par des essais simples, et gardez une copie de vos sauvegardes de jeu.
