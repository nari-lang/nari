class Character {
    constructor(name, health, strength) {
        this.name = name;
        this.health = health;
        this.strength = strength;
    }
}

let hero = new Character("Hero", 100, 10);

console.log(hero);